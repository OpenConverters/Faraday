#pragma once
// Report diff: "did this change make EMC worse?" — the question a review
// tool answers on every revision, and the one a CI gate can hold a brownfield
// board to (gate on REGRESSIONS, not on absolute findings a legacy layout
// already carries).
//
// Findings are matched by IDENTITY, not by report order: (rule, net pair,
// layer pair). IDs like F-0001 are rank-dependent and shift when unrelated
// findings appear. Findings sharing an identity are aggregated (count, worst
// severity, worst numbers) before comparison, so "one more plane crossing on
// the same net" reads as a worsening of that key, not a brand-new issue.
//
// A key WORSENS when its worst severity rank rises, its saturated coupling
// grows by more than 1 dB, its coupled/loop extent grows by more than 10 %
// AND 2 mm, or its instance count rises. The mirror-image thresholds mark
// improvement; inside the thresholds it is unchanged — a 0.2 dB wiggle from
// re-routing is noise, not a regression.

#include <nlohmann/json.hpp>

#include <algorithm>
#include <map>
#include <string>

namespace faraday::diff {

inline int severity_rank(const std::string& label) {
    if (label == "high") return 3;
    if (label == "medium") return 2;
    if (label == "low") return 1;
    return 0;   // info
}

namespace detail {

struct Agg {
    int count = 0;
    int worst_rank = 0;
    std::string worst_label = "info";
    double next_db = -1e30;      // saturated coupling, when the rule has one
    bool has_db = false;
    double len_mm = 0;           // worst coupled length / loop area proxy
    std::string title;           // the worst instance's title
    std::string id;              // ... and its id (current side only)
};

// netA/netB in a report are net IDs — integers assigned per import, NOT
// stable across revisions (one added net shifts every ID after it). Identity
// must use net NAMES, resolved through the report's own nets table.
inline std::map<int, std::string> net_names(const nlohmann::json& report) {
    std::map<int, std::string> out;
    if (report.contains("board") && report["board"].contains("nets"))
        for (const auto& n : report["board"]["nets"])
            out[n.value("id", -1)] = n.value("name", "");
    return out;
}

inline std::string net_of(const nlohmann::json& f, const char* field,
                          const std::map<int, std::string>& names) {
    if (!f.contains(field)) return "";
    const auto& v = f.at(field);
    if (v.is_string()) return v.get<std::string>();
    const int id = v.get<int>();
    auto it = names.find(id);
    return it != names.end() ? it->second : "#" + std::to_string(id);
}

inline std::string key_of(const nlohmann::json& f,
                          const std::map<int, std::string>& names) {
    std::string a = net_of(f, "netA", names), b = net_of(f, "netB", names);
    int ca = f.value("cuA", -1), cb = f.value("cuB", -1);
    if (b < a) { std::swap(a, b); std::swap(ca, cb); }
    return f.value("rule", "?") + "|" + a + "|" + b + "|" +
           std::to_string(ca) + "|" + std::to_string(cb);
}

inline std::map<std::string, Agg> aggregate(const nlohmann::json& report) {
    std::map<std::string, Agg> out;
    const auto names = net_names(report);
    for (const auto& f : report.at("findings")) {
        Agg& a = out[key_of(f, names)];
        ++a.count;
        const std::string label = f.value("severityLabel", "info");
        const int rank = severity_rank(label);
        const bool worst = rank > a.worst_rank || a.count == 1;
        if (worst) {
            a.worst_rank = rank;
            a.worst_label = label;
            a.title = f.value("title", "");
            a.id = f.value("id", "");
        }
        if (f.contains("nextDb")) {
            a.has_db = true;
            a.next_db = std::max(a.next_db, f.at("nextDb").get<double>());
        }
        a.len_mm = std::max(a.len_mm, f.value("coupledLenMm", 0.0));
    }
    return out;
}

}  // namespace detail

inline nlohmann::json diff_reports(const nlohmann::json& base,
                                   const nlohmann::json& cur) {
    const auto ba = detail::aggregate(base);
    const auto ca = detail::aggregate(cur);

    nlohmann::json added = nlohmann::json::array();
    nlohmann::json resolved = nlohmann::json::array();
    nlohmann::json worsened = nlohmann::json::array();
    nlohmann::json improved = nlohmann::json::array();

    for (const auto& [key, c] : ca) {
        auto it = ba.find(key);
        if (it == ba.end()) {
            added.push_back({{"key", key}, {"id", c.id}, {"title", c.title},
                             {"severityLabel", c.worst_label}});
            continue;
        }
        const auto& b = it->second;
        std::string why;
        std::string better;
        if (c.worst_rank > b.worst_rank)
            why = "severity " + b.worst_label + " -> " + c.worst_label;
        else if (c.worst_rank < b.worst_rank)
            better = "severity " + b.worst_label + " -> " + c.worst_label;
        else if (c.has_db && b.has_db && c.next_db > b.next_db + 1.0)
            why = "coupling " + std::to_string(b.next_db).substr(0, 5) +
                  " -> " + std::to_string(c.next_db).substr(0, 5) + " dB";
        else if (c.has_db && b.has_db && c.next_db < b.next_db - 1.0)
            better = "coupling " + std::to_string(b.next_db).substr(0, 5) +
                     " -> " + std::to_string(c.next_db).substr(0, 5) + " dB";
        else if (c.len_mm > b.len_mm * 1.1 && c.len_mm > b.len_mm + 2.0)
            why = "extent " + std::to_string(b.len_mm).substr(0, 5) + " -> " +
                  std::to_string(c.len_mm).substr(0, 5) + " mm";
        else if (b.len_mm > c.len_mm * 1.1 && b.len_mm > c.len_mm + 2.0)
            better = "extent " + std::to_string(b.len_mm).substr(0, 5) +
                     " -> " + std::to_string(c.len_mm).substr(0, 5) + " mm";
        else if (c.count > b.count)
            why = std::to_string(b.count) + " -> " + std::to_string(c.count) +
                  " instances";
        else if (c.count < b.count)
            better = std::to_string(b.count) + " -> " +
                     std::to_string(c.count) + " instances";
        if (!why.empty())
            worsened.push_back({{"key", key}, {"id", c.id},
                                {"title", c.title}, {"why", why},
                                {"severityLabel", c.worst_label}});
        else if (!better.empty())
            improved.push_back({{"key", key}, {"id", c.id},
                                {"title", c.title}, {"why", better},
                                {"severityLabel", c.worst_label}});
    }
    for (const auto& [key, b] : ba)
        if (!ca.count(key))
            resolved.push_back({{"key", key}, {"title", b.title},
                                {"severityLabel", b.worst_label}});

    const std::string verdict =
        (!added.empty() || !worsened.empty()) ? "regression"
        : (!resolved.empty() || !improved.empty()) ? "improved"
                                                   : "unchanged";
    return {{"added", added}, {"resolved", resolved},
            {"worsened", worsened}, {"improved", improved},
            {"verdict", verdict}};
}

// The CI gate: does the diff contain a NEW or WORSENED key at/above the
// given severity?
inline bool has_regression_at(const nlohmann::json& d,
                              const std::string& level) {
    const int min_rank = severity_rank(level);
    for (const char* group : {"added", "worsened"})
        for (const auto& e : d.at(group))
            if (severity_rank(e.value("severityLabel", "info")) >= min_rank)
                return true;
    return false;
}

}  // namespace faraday::diff

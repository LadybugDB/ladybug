#pragma once

// Agent mode helpers for the lbug shell.
//
// Mirrors DuckDB 2.0's agent mode (adapted for lbug): when the CLI detects it
// is talking to an AI coding agent through a pipe, it switches to compact
// Markdown output, explicit truncation footers with result hashes, JSON
// errors, compact EXPLAIN plans and a one-line schema listing.

#include <cstdint>
#include <cstdlib>
#include <regex>
#include <sstream>
#include <string>
#include <vector>

namespace lbug {
namespace main {
namespace agent {

// Environment variables that identify an AI coding agent session.
inline const std::vector<std::string>& agentEnvVars() {
    static const std::vector<std::string> vars = {"AI_AGENT", "AGENT", "CLAUDECODE", "CODEX_CI",
        "CODEX_SANDBOX", "CODEX_THREAD_ID", "CURSOR_AGENT", "GEMINI_CLI", "COPILOT_AGENT",
        "COPILOT_CLI", "COPILOT_AGENT_SESSION_ID"};
    return vars;
}

inline bool isEnvSet(const std::string& name) {
    const char* val = std::getenv(name.c_str());
    return val != nullptr && std::string(val).length() > 0;
}

// Returns the name of the first agent-identifying env var that is set, or "".
inline std::string detectAgentEnv() {
    for (auto& var : agentEnvVars()) {
        if (isEnvSet(var)) {
            return var;
        }
    }
    return "";
}

// Short agent id used by `.show` / `:stats` (e.g. "claude-code").
inline std::string agentIdForEnv(const std::string& env) {
    if (env == "CLAUDECODE") {
        return "claude-code";
    }
    if (env == "CODEX_CI" || env == "CODEX_SANDBOX" || env == "CODEX_THREAD_ID") {
        return "codex";
    }
    if (env == "CURSOR_AGENT") {
        return "cursor";
    }
    if (env == "GEMINI_CLI") {
        return "gemini-cli";
    }
    if (env == "COPILOT_AGENT" || env == "COPILOT_CLI" || env == "COPILOT_AGENT_SESSION_ID") {
        return "copilot";
    }
    if (env == "AI_AGENT" || env == "AGENT") {
        return "agent";
    }
    if (!env.empty()) {
        std::string lower = env;
        for (auto& c : lower) {
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
        return lower;
    }
    return "flag";
}

inline bool envForcesAgentOn() {
    const char* val = std::getenv("LBUG_AGENT_MODE");
    if (val == nullptr) {
        return false;
    }
    std::string v = val;
    for (auto& c : v) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return v == "1" || v == "true" || v == "yes" || v == "on";
}

inline std::string jsonEscape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 2);
    for (char c : s) {
        switch (c) {
        case '"':
            out += "\\\"";
            break;
        case '\\':
            out += "\\\\";
            break;
        case '\n':
            out += "\\n";
            break;
        case '\r':
            out += "\\r";
            break;
        case '\t':
            out += "\\t";
            break;
        default:
            if (static_cast<unsigned char>(c) < 0x20) {
                char buf[8];
                snprintf(buf, sizeof(buf), "\\u%04x", c);
                out += buf;
            } else {
                out += c;
            }
        }
    }
    return out;
}

// Escape a cell value for a Markdown table: pipes would break the row and raw
// newlines would break the table, so both are escaped.
inline std::string markdownEscapeCell(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        if (c == '|') {
            out += "\\|";
        } else if (c == '\n') {
            out += "\\n";
        } else if (c == '\r') {
            out += "\\r";
        } else {
            out += c;
        }
    }
    return out;
}

// Number of Unicode code points in a UTF-8 string.
inline size_t utf8Length(const std::string& s) {
    size_t len = 0;
    for (size_t i = 0; i < s.size();) {
        unsigned char c = static_cast<unsigned char>(s[i]);
        size_t step = 1;
        if ((c & 0x80) == 0) {
            step = 1;
        } else if ((c & 0xE0) == 0xC0) {
            step = 2;
        } else if ((c & 0xF0) == 0xE0) {
            step = 3;
        } else if ((c & 0xF8) == 0xF0) {
            step = 4;
        }
        i += step;
        len++;
    }
    return len;
}

// Truncate to at most maxChars code points without splitting a UTF-8 sequence.
// Sets omitted to the number of dropped code points.
inline std::string utf8Truncate(const std::string& s, size_t maxChars, size_t& omitted) {
    omitted = 0;
    size_t total = utf8Length(s);
    if (total <= maxChars) {
        return s;
    }
    omitted = total - maxChars;
    size_t kept = 0;
    size_t i = 0;
    while (i < s.size() && kept < maxChars) {
        unsigned char c = static_cast<unsigned char>(s[i]);
        size_t step = 1;
        if ((c & 0x80) == 0) {
            step = 1;
        } else if ((c & 0xE0) == 0xC0) {
            step = 2;
        } else if ((c & 0xF0) == 0xE0) {
            step = 3;
        } else if ((c & 0xF8) == 0xF0) {
            step = 4;
        }
        i += step;
        kept++;
    }
    // Guard against malformed input running past the end.
    if (i > s.size()) {
        i = s.size();
    }
    return s.substr(0, i);
}

// 64-bit FNV-1a over raw bytes.
inline uint64_t fnv1a(const char* data, size_t len, uint64_t hash = 1469598103934665603ULL) {
    for (size_t i = 0; i < len; i++) {
        hash ^= static_cast<uint64_t>(static_cast<unsigned char>(data[i]));
        hash *= 1099511628211ULL;
    }
    return hash;
}

inline std::string hashHex(uint64_t hash) {
    char buf[17];
    snprintf(buf, sizeof(buf), "%016llx", static_cast<unsigned long long>(hash));
    return std::string(buf);
}

// Split "X exception: rest" into ("X", "rest"). Shell-level errors use "Shell".
inline std::string exceptionTypeFromMessage(const std::string& msg, std::string& stripped) {
    static const std::vector<std::string> prefixes = {"Binder exception: ", "Parser exception: ",
        "Catalog exception: ", "Connection exception: ", "Conversion exception: ",
        "Copy exception: ", "Extension exception: ", "Internal exception: ",
        "Interrupt exception: ", "IO exception: ", "Not implemented exception: ",
        "Optimizer exception: ", "Out of Range exception: ", "Runtime exception: ",
        "Storage exception: ", "Transaction exception: ", "HTTP exception: "};
    for (auto& prefix : prefixes) {
        if (msg.compare(0, prefix.size(), prefix) == 0) {
            stripped = msg.substr(prefix.size());
            auto type = prefix.substr(0, prefix.find(' '));
            // "Out of Range" -> "OutOfRange" style single token.
            std::string compact;
            for (char c : type) {
                if (c != ' ') {
                    compact += c;
                }
            }
            return compact;
        }
    }
    stripped = msg;
    return "Unknown";
}

inline std::string formatErrorJson(const std::string& type, const std::string& message) {
    std::ostringstream oss;
    oss << "{\"exception_type\": \"" << jsonEscape(type) << "\", \"exception_message\": \""
        << jsonEscape(message) << "\"}";
    return oss.str();
}

inline std::string shellErrorJson(const std::string& message) {
    return formatErrorJson("Shell", message);
}

namespace detail {

inline std::string trimBox(const std::string& line) {
    size_t start = 0;
    size_t end = line.size();
    while (start < end) {
        unsigned char c = static_cast<unsigned char>(line[start]);
        bool isBoxRune = (c == 0xE2 && start + 2 < end &&
                          (static_cast<unsigned char>(line[start + 1]) == 0x94 ||
                              static_cast<unsigned char>(line[start + 1]) == 0x95));
        if (c == ' ' || c == '\t' || isBoxRune) {
            if (c == 0xE2) {
                start += 3; // box-drawing rune (3 bytes in UTF-8)
            } else {
                start++;
            }
        } else if (c == '|') {
            start++;
        } else {
            break;
        }
    }
    while (end > start) {
        unsigned char c = static_cast<unsigned char>(line[end - 1]);
        if (c == ' ' || c == '\t' || c == '|') {
            end--;
        } else if (end >= 3 && static_cast<unsigned char>(line[end - 3]) == 0xE2 &&
                   (static_cast<unsigned char>(line[end - 2]) == 0x94 ||
                       static_cast<unsigned char>(line[end - 2]) == 0x95)) {
            end -= 3; // box-drawing rune
        } else {
            break;
        }
    }
    auto s = line.substr(start, end - start);
    // Trim ASCII whitespace left over.
    auto first = s.find_first_not_of(" \t");
    if (first == std::string::npos) {
        return "";
    }
    auto last = s.find_last_not_of(" \t");
    return s.substr(first, last - first + 1);
}

inline bool isSeparatorLine(const std::string& trimmed) {
    if (trimmed.empty()) {
        return true;
    }
    for (size_t i = 0; i < trimmed.size();) {
        unsigned char c = static_cast<unsigned char>(trimmed[i]);
        if (c == '-' || c == ' ') {
            i++;
            continue;
        }
        if (c == 0xE2 && i + 2 < trimmed.size() &&
            (static_cast<unsigned char>(trimmed[i + 1]) == 0x94 ||
                static_cast<unsigned char>(trimmed[i + 1]) == 0x95)) {
            i += 3;
            continue;
        }
        return false;
    }
    return true;
}

inline bool isProfilingLine(const std::string& trimmed) {
    return trimmed.compare(0, 16, "NumOutputTuples:") == 0 ||
           trimmed.compare(0, 14, "ExecutionTime:") == 0 ||
           trimmed.compare(0, 10, "TotalTime:") == 0;
}

} // namespace detail

// Convert the box-art plan produced by EXPLAIN into a compact one-operator-
// per-line indented plan. Boxes printed on the same text row sit at the same
// depth; each new row of boxes goes one level deeper.
inline std::string compactPlan(const std::string& planText) {
    static const std::regex headerRegex(R"((?:^|\s)([A-Z][A-Z_0-9]*)\[(\d+)\])");
    // PROFILE box plans contain a WallClockTime attribute on the root PROFILE
    // operator; plain EXPLAIN plans do not.
    const bool isProfile = planText.find("WallClockTime:") != std::string::npos;
    struct Entry {
        std::string name;
        std::vector<std::string> props;
        size_t topLine = 0;
        size_t spanStart = 0;
        size_t spanEnd = std::string::npos;
        int depth = 0;
    };
    std::vector<std::string> lines;
    {
        std::istringstream iss(planText);
        std::string line;
        while (std::getline(iss, line)) {
            if (!line.empty() && line.back() == '\r') {
                line.pop_back();
            }
            lines.push_back(line);
        }
    }
    std::vector<Entry> entries;
    for (size_t i = 0; i < lines.size(); i++) {
        // One line can hold several side-by-side operators; collect them all.
        // The plan title box ("Physical Plan") has no [id]; anything matching
        // the regex is a real operator.
        std::sregex_iterator it(lines[i].begin(), lines[i].end(), headerRegex);
        std::sregex_iterator done;
        for (; it != done; ++it) {
            Entry entry;
            entry.name = (*it)[1].str();
            // Column span of this operator's box, so side-by-side boxes on the
            // same text row do not bleed into each other's properties.
            static const std::string vert = "\u2502";
            size_t begin = (size_t)(*it).position(0);
            size_t open = lines[i].rfind(vert, begin);
            size_t close = lines[i].find(vert, begin + (*it).length(0));
            if (open != std::string::npos) {
                entry.spanStart = open + vert.size();
            }
            if (close != std::string::npos) {
                entry.spanEnd = close;
            }
            entry.topLine = i;
            for (size_t j = i; j > 0; j--) {
                if (lines[j - 1].find("\u250c") != std::string::npos) {
                    entry.topLine = j - 1;
                    break;
                }
            }
            entries.push_back(std::move(entry));
        }
    }
    // Property collection per operator: lines until the box bottom border or
    // the next operator header, clipped to the operator's column span.
    for (auto& entry : entries) {
        size_t i = entry.topLine;
        for (; i < lines.size(); i++) {
            if (lines[i].find(entry.name + "[") != std::string::npos) {
                break;
            }
        }
        for (size_t j = i + 1; j < lines.size(); j++) {
            std::smatch next;
            if (std::regex_search(lines[j], next, headerRegex)) {
                break;
            }
            if (lines[j].find("\u2514") != std::string::npos ||
                lines[j].find("\u2518") != std::string::npos) {
                break;
            }
            std::string seg = lines[j];
            if (entry.spanEnd != std::string::npos && entry.spanEnd <= seg.size() &&
                entry.spanStart < entry.spanEnd) {
                seg = seg.substr(entry.spanStart, entry.spanEnd - entry.spanStart);
            }
            auto trimmed = detail::trimBox(seg);
            if (detail::isSeparatorLine(trimmed)) {
                continue;
            }
            // Plain EXPLAIN plans carry zeroed profiling counters (the profiler is
            // disabled); hide them to keep the compact plan readable. PROFILE plans
            // carry real per-operator timings, so preserve them.
            if (!isProfile && detail::isProfilingLine(trimmed)) {
                continue;
            }
            if (trimmed.empty()) {
                continue;
            }
            if (trimmed.find(':') != std::string::npos || entry.props.empty()) {
                entry.props.push_back(trimmed);
            } else {
                entry.props.back() += ", " + trimmed;
            }
        }
    }
    // Depth: each new box-top row goes one level deeper; boxes sharing a
    // top-border line sit at the same depth.
    {
        int depth = -1;
        size_t lastTop = static_cast<size_t>(-1);
        for (auto& entry : entries) {
            if (entry.topLine != lastTop) {
                depth++;
                lastTop = entry.topLine;
            }
            entry.depth = depth;
        }
    }
    std::ostringstream oss;
    for (auto& entry : entries) {
        oss << std::string(static_cast<size_t>(entry.depth) * 2, ' ') << entry.name;
        if (!entry.props.empty()) {
            oss << " ";
            for (size_t k = 0; k < entry.props.size(); k++) {
                if (k > 0) {
                    oss << "; ";
                }
                oss << entry.props[k];
            }
        }
        oss << "\n";
    }
    return oss.str();
}

} // namespace agent
} // namespace main
} // namespace lbug

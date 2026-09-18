// ProfilingInstrumentor.cpp — Pass 1 implementation (REQ-PR1, REQ-PR2, REQ-PR4).
//
// See ProfilingInstrumentor.h for the full design rationale and documented
// limitations of this text-level transformer.

#include "ProfilingInstrumentor.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <map>
#include <sstream>

// ---------------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------------

bool ProfilingInstrumentor::parseNumericLiteral(const std::string& s, long& out) {
    // Trim surrounding whitespace.
    std::size_t begin = 0;
    std::size_t end = s.size();
    while (begin < end && std::isspace(static_cast<unsigned char>(s[begin]))) ++begin;
    while (end > begin && std::isspace(static_cast<unsigned char>(s[end - 1]))) --end;
    if (begin >= end) return false;

    std::size_t idx = begin;
    if (s[idx] == '-' || s[idx] == '+') ++idx;
    if (idx >= end) return false;
    for (std::size_t k = idx; k < end; ++k) {
        if (!std::isdigit(static_cast<unsigned char>(s[k]))) return false;
    }

    try {
        out = std::stol(s.substr(begin, end - begin));
    } catch (...) {
        return false;
    }
    return true;
}

std::string ProfilingInstrumentor::escapeForCxxLiteral(const std::string& s) {
    std::string r;
    r.reserve(s.size());
    for (char c : s) {
        if (c == '\\' || c == '"') r.push_back('\\');
        r.push_back(c);
    }
    return r;
}

std::size_t ProfilingInstrumentor::lineIndexOfOffset(const std::vector<std::size_t>& lineStarts,
                                                       std::size_t offset) {
    auto it = std::upper_bound(lineStarts.begin(), lineStarts.end(), offset);
    std::size_t idx = static_cast<std::size_t>(it - lineStarts.begin());
    return (idx == 0) ? 0 : (idx - 1);
}

// ---------------------------------------------------------------------------
// Static compile-time estimates (REQ-PR2)
// ---------------------------------------------------------------------------
//
// trip_count:
//   - tripBound is a numeric literal ("1024")           -> use it directly.
//   - tripBound is a non-constant expression ("N", "n+1") -> conservative
//     default estimate of 1 (documented limitation: without a constant
//     folder over the full expression AST, "unknown" is the only honest
//     answer; 1 keeps downstream arithmetic well-defined and, combined
//     with REQ-G4's static fallback rule, still routes such loops to
//     CPU_PREFERRED rather than overselling them as GPU-profitable).
//
// flops:
//   - accessCount * 2 is used as a rough proxy for arithmetic work per
//     iteration (ClangBridge's IR does not carry full expression ASTs, so
//     the true op count is unknowable here), then scaled by the same
//     trip-count multiplier used for the byte estimates below so that
//     flops/bytes both represent *whole-loop-execution* totals (matching
//     the magnitudes in the REQ-PR3 example schema, where flops is ~1e8
//     for a 5e7-trip loop with a handful of accesses per iteration).
//
// bytes_read / bytes_written:
//   - 8 bytes (assume double) per read/write ArrayAccess, multiplied by
//     the same trip-count multiplier (numeric tripBound if available,
//     else 1) per the task spec.
ProfilingInstrumentor::LoopEstimate ProfilingInstrumentor::estimateLoop(const LoopNest& loop) {
    LoopEstimate est;

    long bound = 0;
    bool numeric = parseNumericLiteral(loop.tripBound, bound);
    est.tripCount = numeric ? bound : 1;
    long multiplier = est.tripCount;

    long accessCount = static_cast<long>(loop.arrayAccesses.size());
    long reads = 0;
    long writes = 0;
    for (const ArrayAccess& a : loop.arrayAccesses) {
        if (a.isWrite) ++writes;
        else ++reads;
    }

    est.flops = accessCount * 2 * multiplier;
    est.bytesRead = reads * 8 * multiplier;
    est.bytesWritten = writes * 8 * multiplier;
    return est;
}

// ---------------------------------------------------------------------------
// instrument()
// ---------------------------------------------------------------------------
std::string ProfilingInstrumentor::instrument(const std::string& originalSourcePath,
                                               const ProgramIR& ir,
                                               const std::set<std::string>& parallelSafeLoopLocations) {
    std::ifstream in(originalSourcePath, std::ios::binary);
    if (!in) {
        // Can't read the original source — return a minimal, still
        // compilable stub rather than throwing. The caller can decide how
        // to surface this (e.g. a warning on stderr).
        return "#include \"profiler.h\"\n";
    }

    std::ostringstream rawBuf;
    rawBuf << in.rdbuf();
    std::string raw = rawBuf.str();

    // Normalize line endings (drop CR so CRLF becomes LF).
    std::string text;
    text.reserve(raw.size());
    for (char c : raw) {
        if (c != '\r') text.push_back(c);
    }

    // Split into lines, recording each line's starting byte offset in
    // `text`. lines[i] holds the content of line (i+1) without its
    // trailing newline.
    std::vector<std::string> lines;
    std::vector<std::size_t> lineStarts;
    std::size_t start = 0;
    for (std::size_t i = 0; i <= text.size(); ++i) {
        if (i == text.size() || text[i] == '\n') {
            lineStarts.push_back(start);
            lines.push_back(text.substr(start, i - start));
            start = i + 1;
        }
    }

    // lineNo (1-based) -> lines to insert immediately before/after it.
    std::map<int, std::vector<std::string>> insertBefore;
    std::map<int, std::vector<std::string>> insertAfter;

    for (const auto& fnEntry : ir) {
        const FunctionIR& fn = fnEntry.second;
        for (const LoopNest& loop : fn.loops) {
            if (parallelSafeLoopLocations.find(loop.location) == parallelSafeLoopLocations.end()) {
                continue;
            }

            // Parse "filename:line" -> line number (1-based).
            std::size_t colonPos = loop.location.rfind(':');
            if (colonPos == std::string::npos) continue;
            long lineNoLong = 0;
            try {
                lineNoLong = std::stol(loop.location.substr(colonPos + 1));
            } catch (...) {
                continue;
            }
            if (lineNoLong <= 0) continue;
            std::size_t lineIdx0 = static_cast<std::size_t>(lineNoLong - 1);
            if (lineIdx0 >= lines.size()) continue; // location doesn't match this file's line count

            // Locate the "for" keyword (best-effort: search the reported
            // line only) and its opening paren.
            const std::string& headerLine = lines[lineIdx0];
            std::size_t relFor = headerLine.find("for");
            std::size_t forAbs = (relFor != std::string::npos)
                                      ? (lineStarts[lineIdx0] + relFor)
                                      : lineStarts[lineIdx0];
            std::size_t parenOpen = text.find('(', forAbs);
            if (parenOpen == std::string::npos) continue; // can't instrument; skip this loop

            // Match the loop header's parentheses.
            int depth = 0;
            std::size_t i = parenOpen;
            for (; i < text.size(); ++i) {
                if (text[i] == '(') {
                    ++depth;
                } else if (text[i] == ')') {
                    --depth;
                    if (depth == 0) break;
                }
            }
            if (depth != 0) continue; // unmatched parens; skip this loop
            std::size_t parenClose = i;

            // Find the first non-whitespace character after the header.
            std::size_t bodyStart = text.find_first_not_of(" \t\r\n", parenClose + 1);

            std::size_t endLineIdx0;
            if (bodyStart != std::string::npos && text[bodyStart] == '{') {
                // Brace-delimited body: find the matching closing brace.
                int bd = 0;
                std::size_t j = bodyStart;
                for (; j < text.size(); ++j) {
                    if (text[j] == '{') {
                        ++bd;
                    } else if (text[j] == '}') {
                        --bd;
                        if (bd == 0) break;
                    }
                }
                endLineIdx0 = (bd == 0) ? lineIndexOfOffset(lineStarts, j)
                                        : (lines.empty() ? 0 : lines.size() - 1);
            } else if (bodyStart != std::string::npos) {
                // Single-statement (brace-less) body: end at the next ';'.
                std::size_t semi = text.find(';', bodyStart);
                endLineIdx0 = (semi != std::string::npos)
                                   ? lineIndexOfOffset(lineStarts, semi)
                                   : (lines.empty() ? 0 : lines.size() - 1);
            } else {
                // Nothing follows the header at all; fall back to the
                // loop's own line so we still emit *something* sane.
                endLineIdx0 = lineIdx0;
            }

            int startLineNo = static_cast<int>(lineIdx0) + 1;
            int endLineNo = static_cast<int>(endLineIdx0) + 1;

            LoopEstimate est = estimateLoop(loop);

            // Reuse the loop header line's leading whitespace so the
            // inserted calls read naturally in the generated output.
            std::string indent;
            {
                const std::string& l = lines[lineIdx0];
                std::size_t k = 0;
                while (k < l.size() && (l[k] == ' ' || l[k] == '\t')) ++k;
                indent = l.substr(0, k);
            }

            std::string locLit = escapeForCxxLiteral(loop.location);

            std::ostringstream startCall;
            startCall << indent << "profile_loop_start(\"" << locLit << "\");";

            std::ostringstream endCall;
            endCall << indent << "profile_loop_end(\"" << locLit << "\", "
                    << est.tripCount << "L, " << est.flops << "L, "
                    << est.bytesRead << "L, " << est.bytesWritten << "L);";

            insertBefore[startLineNo].push_back(startCall.str());
            insertAfter[endLineNo].push_back(endCall.str());
        }
    }

    // Rebuild the output: `#include "profiler.h"` up top, then the
    // original lines with insertions spliced in around each instrumented
    // loop.
    std::ostringstream out;
    out << "#include \"profiler.h\"\n\n";
    for (std::size_t li = 0; li < lines.size(); ++li) {
        int lineNo = static_cast<int>(li) + 1;

        auto bIt = insertBefore.find(lineNo);
        if (bIt != insertBefore.end()) {
            for (const std::string& s : bIt->second) out << s << "\n";
        }

        out << lines[li] << "\n";

        auto aIt = insertAfter.find(lineNo);
        if (aIt != insertAfter.end()) {
            for (const std::string& s : aIt->second) out << s << "\n";
        }
    }

    return out.str();
}

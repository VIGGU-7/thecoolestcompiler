// ParallelismChecker.cpp — Implementation of Task 5 (loop parallelism
// classification). See the design note at the top of ParallelismChecker.h
// for why this operates on ClangBridge text (ArrayAccess::indexExpr) rather
// than ROSE's DepInfoAnal, and for the REQ-P5 reduction-detection caveat.

#include "ParallelismChecker.h"

#include <algorithm>
#include <cctype>
#include <regex>
#include <unordered_set>

// ─────────────────────────────────────────────────────────────────────────
// toString(LoopClassification)
// ─────────────────────────────────────────────────────────────────────────
std::string toString(LoopClassification c) {
    switch (c) {
        case PARALLEL_SAFE:         return "PARALLEL_SAFE";
        case REDUCTION_CANDIDATE:   return "REDUCTION_CANDIDATE";
        case SERIAL_ONLY:           return "SERIAL_ONLY";
        case UNKNOWN_CONSERVATIVE:  return "UNKNOWN_CONSERVATIVE";
    }
    return "UNKNOWN_CONSERVATIVE";
}

namespace {

// Escape regex metacharacters in an identifier. Induction variables are
// ordinary C identifiers in practice, but we escape defensively rather than
// assume it.
std::string regexEscapeIdentifier(const std::string& s) {
    static const std::string special = "\\^$.|?*+()[]{}";
    std::string out;
    out.reserve(s.size());
    for (char ch : s) {
        if (special.find(ch) != std::string::npos) {
            out.push_back('\\');
        }
        out.push_back(ch);
    }
    return out;
}

// Lowercase copy, for case-insensitive substring matching in the reduction
// heuristic.
std::string toLower(const std::string& s) {
    std::string out = s;
    std::transform(out.begin(), out.end(), out.begin(),
                    [](unsigned char c) { return std::tolower(c); });
    return out;
}

}  // namespace

// ─────────────────────────────────────────────────────────────────────────
// parseAffineIndex — REQ-P1 / REQ-P4
//
// Recognizes exactly: optional whitespace, the induction variable, an
// optional "+K" / "-K" constant offset (whitespace-tolerant), optional
// trailing whitespace — matched against the *entire* indexExpr string. Any
// other shape (a different variable, "i*2+j", "f(i)", a bare numeric
// literal, etc.) is reported as non-affine so the caller can fall back to
// UNKNOWN_CONSERVATIVE.
// ─────────────────────────────────────────────────────────────────────────
ParallelismChecker::AffineIndex ParallelismChecker::parseAffineIndex(
    const std::string& indexExpr, const std::string& inductionVar) {
    if (inductionVar.empty()) {
        return {false, 0};
    }

    std::string pattern = "^\\s*" + regexEscapeIdentifier(inductionVar) +
                           "\\s*([+-]\\s*\\d+)?\\s*$";
    std::regex re(pattern);

    std::smatch m;
    if (!std::regex_match(indexExpr, m, re)) {
        return {false, 0};
    }

    if (!m[1].matched) {
        return {true, 0};
    }

    std::string offStr = m[1].str();
    offStr.erase(std::remove_if(offStr.begin(), offStr.end(),
                                 [](unsigned char c) { return std::isspace(c); }),
                 offStr.end());
    long offset = 0;
    try {
        offset = std::stol(offStr);
    } catch (...) {
        // Shouldn't happen given the regex, but stay conservative.
        return {false, 0};
    }
    return {true, offset};
}

// ─────────────────────────────────────────────────────────────────────────
// checkArrayDependences — REQ-P1 (loop-carried dependence) / REQ-P4
// (non-affine => conservative)
// ─────────────────────────────────────────────────────────────────────────
void ParallelismChecker::checkArrayDependences(const LoopNest& loop,
                                                bool& serial,
                                                bool& unknown,
                                                std::string& reasonOut) {
    serial = false;
    unknown = false;

    // Group accesses by array name, preserving encounter order.
    std::map<std::string, std::vector<const ArrayAccess*>> byArray;
    for (const ArrayAccess& a : loop.arrayAccesses) {
        byArray[a.arrayName].push_back(&a);
    }

    bool anyUnknown = false;
    std::string unknownReason;

    for (const auto& kv : byArray) {
        const std::string& arrayName = kv.first;
        const std::vector<const ArrayAccess*>& accesses = kv.second;

        std::vector<std::pair<long, bool>> affine;  // (offset, isWrite)
        affine.reserve(accesses.size());

        for (const ArrayAccess* a : accesses) {
            AffineIndex parsed = parseAffineIndex(a->indexExpr, loop.inductionVar);
            if (!parsed.isAffine) {
                if (!anyUnknown) {
                    anyUnknown = true;
                    unknownReason = "array '" + arrayName +
                        "' has a non-affine or unrecognized index expression ('" +
                        a->indexExpr + "') relative to induction variable '" +
                        loop.inductionVar + "'";
                }
                continue;
            }
            affine.emplace_back(parsed.offset, a->isWrite);
        }

        // Pairwise check for loop-carried dependence: two accesses to the
        // SAME array at DIFFERENT constant offsets, at least one a write,
        // means iteration k and iteration k+delta touch the same address
        // (RAW/WAR/WAW across iterations).
        for (size_t i = 0; i < affine.size() && !serial; ++i) {
            for (size_t j = i + 1; j < affine.size(); ++j) {
                bool differentOffsets = affine[i].first != affine[j].first;
                bool eitherWrite = affine[i].second || affine[j].second;
                if (differentOffsets && eitherWrite) {
                    serial = true;
                    reasonOut = "loop-carried dependence on array '" + arrayName +
                        "': offset " + std::to_string(affine[i].first) +
                        " vs offset " + std::to_string(affine[j].first) +
                        " (induction variable '" + loop.inductionVar + "')";
                    break;
                }
            }
        }

        if (serial) {
            return;  // Definite answer — no need to keep scanning.
        }
    }

    if (anyUnknown) {
        unknown = true;
        reasonOut = unknownReason;
    }
}

// ─────────────────────────────────────────────────────────────────────────
// checkCallSiteEffects — REQ-P2
// ─────────────────────────────────────────────────────────────────────────
void ParallelismChecker::checkCallSiteEffects(
    const LoopNest& loop, const FunctionIR& enclosingFunc,
    const InterproceduralAnalysis& interproc, bool& serial, bool& unknown,
    std::string& reasonOut) {
    serial = false;
    unknown = false;

    if (loop.callSites.empty()) {
        return;
    }

    // All array names touched anywhere in the loop (conservative proxy for
    // "any variable referenced across iterations" — we have no per-iteration
    // variable renaming from text alone, so any array in the loop counts).
    std::unordered_set<std::string> loopArrayNames;
    for (const ArrayAccess& a : loop.arrayAccesses) {
        loopArrayNames.insert(a.arrayName);
    }

    // Parameter names of the enclosing function — arrays NOT in this set are
    // assumed to possibly be globals/file-scope for the "modifiesGlobals"
    // check below (we don't have precise scope info from the IR).
    std::unordered_set<std::string> paramNames;
    for (const ParamIR& p : enclosingFunc.params) {
        paramNames.insert(p.name);
    }
    bool loopTouchesNonParamData = false;
    for (const std::string& name : loopArrayNames) {
        if (paramNames.find(name) == paramNames.end()) {
            loopTouchesNonParamData = true;
            break;
        }
    }

    bool anyUnknownCallee = false;
    std::string unknownReason;

    for (const CallSite& cs : loop.callSites) {
        const FunctionSummary& summary = interproc.getSummary(cs.calleeName);

        if (summary.isPure) {
            continue;  // No effect on user-visible data — ignore.
        }

        // Note: we deliberately reason directly off getSummary()'s fields
        // here rather than calling interproc.calleeModifiesAny(), because
        // calleeModifiesAny() returns true outright for ANY callee with no
        // summary entry at all (its own documented conservative default),
        // which would collapse the "truly unresolved, no info" case into
        // the same answer as "known and proven to overlap" — destroying the
        // SERIAL_ONLY vs UNKNOWN_CONSERVATIVE distinction REQ-P2/REQ-P4
        // require. getSummary() already returns an equivalent conservative
        // FunctionSummary (isConservative=true, modifiesGlobals=true,
        // modSet empty) for unresolved callees, so checking modSet overlap
        // and isConservative directly gives us the finer-grained answer.
        bool overlapsModSet = false;
        for (const std::string& name : loopArrayNames) {
            if (summary.modSet.count(name)) {
                overlapsModSet = true;
                break;
            }
        }

        if (overlapsModSet) {
            serial = true;
            reasonOut = "call to '" + cs.calleeName +
                "' may modify array(s) accessed elsewhere in the loop body";
            return;
        }

        if (summary.modifiesGlobals && loopTouchesNonParamData) {
            serial = true;
            reasonOut = "call to '" + cs.calleeName +
                "' modifies globals and the loop body accesses non-parameter "
                "(possibly global) data";
            return;
        }

        if (summary.isConservative && !anyUnknownCallee) {
            anyUnknownCallee = true;
            unknownReason = "call to '" + cs.calleeName +
                "' has an unresolved or conservatively-approximated "
                "interprocedural summary";
        }
    }

    if (anyUnknownCallee) {
        unknown = true;
        reasonOut = unknownReason;
    }
}

// ─────────────────────────────────────────────────────────────────────────
// detectReduction — REQ-P5 (best-effort heuristic; see header comment)
// ─────────────────────────────────────────────────────────────────────────
LoopVerdict ParallelismChecker::detectReduction(const LoopNest& loop) {
    LoopVerdict v;
    v.classification = PARALLEL_SAFE;  // sentinel: "no reduction detected"

    for (const CallSite& cs : loop.callSites) {
        std::string lname = toLower(cs.calleeName);
        if (lname.find("accumulate") != std::string::npos ||
            lname.find("reduce") != std::string::npos) {
            v.classification = REDUCTION_CANDIDATE;
            v.reductionVar = cs.argExprs.empty() ? "<unknown>" : cs.argExprs.front();
            v.reductionOp = "+";  // best-effort default; text alone can't
                                   // distinguish +=, *=, min/max reductions.
            v.reason = "call site '" + cs.calleeName +
                "' matches reduction/accumulation helper-name heuristic";
            return v;
        }
    }

    return v;
}

// ─────────────────────────────────────────────────────────────────────────
// classify — REQ-P3 (combine REQ-P1/P2/P4 signals, then REQ-P5)
//
// Priority when multiple signals fire: a *definite* SERIAL_ONLY finding
// always wins over an UNKNOWN_CONSERVATIVE finding (both mean "don't
// transform this loop", but SERIAL_ONLY is the more informative, provable
// answer). REDUCTION_CANDIDATE is only considered once the loop is
// otherwise clear of both.
// ─────────────────────────────────────────────────────────────────────────
LoopVerdict ParallelismChecker::classify(const LoopNest& loop,
                                          const FunctionIR& enclosingFunc,
                                          const InterproceduralAnalysis& interproc) const {
    LoopVerdict verdict;

    // ── REQ-P1 / REQ-P4: array index dependence analysis ──────────────────
    bool arraySerial = false, arrayUnknown = false;
    std::string arrayReason;
    checkArrayDependences(loop, arraySerial, arrayUnknown, arrayReason);

    if (arraySerial) {
        verdict.classification = SERIAL_ONLY;
        verdict.reason = arrayReason;
        return verdict;
    }

    // ── REQ-P2: callee mod/ref effects ─────────────────────────────────────
    bool callSerial = false, callUnknown = false;
    std::string callReason;
    checkCallSiteEffects(loop, enclosingFunc, interproc, callSerial, callUnknown,
                          callReason);

    if (callSerial) {
        verdict.classification = SERIAL_ONLY;
        verdict.reason = callReason;
        return verdict;
    }

    if (arrayUnknown) {
        verdict.classification = UNKNOWN_CONSERVATIVE;
        verdict.reason = arrayReason;
        return verdict;
    }

    if (callUnknown) {
        verdict.classification = UNKNOWN_CONSERVATIVE;
        verdict.reason = callReason;
        return verdict;
    }

    // ── REQ-P5: best-effort reduction heuristic ────────────────────────────
    LoopVerdict reduction = detectReduction(loop);
    if (reduction.classification == REDUCTION_CANDIDATE) {
        return reduction;
    }

    // ── Default: no dependence found, no unsafe callee ─────────────────────
    verdict.classification = PARALLEL_SAFE;
    verdict.reason = "no loop-carried dependence detected and all call sites "
                      "are pure or side-effect-free w.r.t. loop data";
    return verdict;
}

// ─────────────────────────────────────────────────────────────────────────
// classifyAll — whole-program driver
// ─────────────────────────────────────────────────────────────────────────
std::map<std::string, std::vector<LoopVerdict>> ParallelismChecker::classifyAll(
    const ProgramIR& ir, const InterproceduralAnalysis& interproc) const {
    std::map<std::string, std::vector<LoopVerdict>> results;

    for (const auto& kv : ir) {
        const FunctionIR& fir = kv.second;
        if (!fir.hasDefinition || fir.loops.empty()) {
            continue;
        }
        std::vector<LoopVerdict> verdicts;
        verdicts.reserve(fir.loops.size());
        for (const LoopNest& loop : fir.loops) {
            verdicts.push_back(classify(loop, fir, interproc));
        }
        results[fir.name] = std::move(verdicts);
    }

    return results;
}

// ─────────────────────────────────────────────────────────────────────────
// printReport — `--check-parallel` CLI driver
// ─────────────────────────────────────────────────────────────────────────
void ParallelismChecker::printReport(const ProgramIR& ir,
                                      const InterproceduralAnalysis& interproc,
                                      std::ostream& out) const {
    for (const auto& kv : ir) {
        const FunctionIR& fir = kv.second;
        if (!fir.hasDefinition) {
            continue;
        }
        for (const LoopNest& loop : fir.loops) {
            LoopVerdict v = classify(loop, fir, interproc);
            out << loop.location << ": " << toString(v.classification)
                << " (" << v.reason << ")";
            if (v.classification == REDUCTION_CANDIDATE) {
                out << " [reduction(" << v.reductionOp << ":" << v.reductionVar
                    << ")]";
            }
            out << "\n";
        }
    }
}

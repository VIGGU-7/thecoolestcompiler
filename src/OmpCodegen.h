#pragma once
// OmpCodegen.h — OpenMP pragma text generation and AST insertion (Task 8)
//
// Consumes the per-loop parallelization decisions (produced elsewhere by the
// GPU-suitability / parallelism-checking passes, Tasks 5-7) and, for each
// loop that should be transformed, builds the appropriate `#pragma omp ...`
// text (REQ-C1/C2/C3/C4) and inserts it immediately before the loop's
// SgForStatement in the ROSE AST (Decision 4 in CONTEXT.md), using
// RoseBridge::loopMap to resolve "file:line" locations to SgForStatement*.
//
// This header intentionally does NOT depend on the real decision-type
// headers (GpuDecision / ParallelismChecker) being written by other agents
// in parallel — it defines its own minimal OmpDecision struct so that this
// module can be implemented and compiled independently. The caller (Task 9,
// translator.cpp main()) is responsible for converting the real decision
// types into OmpDecision when wiring everything together.

#include "ClangBridge.h"
#include "RoseBridge.h"

#include <map>
#include <string>

// ---------------------------------------------------------------------------
// OmpDecision — minimal, decoupled decision record for one loop.
//
// Only loops that SHOULD be transformed at all are expected to appear in the
// `decisions` map passed to applyPragmas(). Loops classified SERIAL_ONLY or
// UNKNOWN_CONSERVATIVE by upstream analysis simply must not be placed in the
// map by the caller — that (plus applyPragmas() only ever touching loops
// present in the map) is what satisfies REQ-C5.
// ---------------------------------------------------------------------------
struct OmpDecision {
    bool gpuProfitable = false;   // true  => "target teams distribute parallel for" (REQ-C1)
                                   // false => "parallel for"                        (REQ-C3)
                                   // (caller only supplies loops known to be parallel-safe)
    bool hasReduction = false;    // true => append a reduction(...) clause (REQ-C2)
    std::string reductionVar;     // reduction variable name (used iff hasReduction)
    std::string reductionOp;      // reduction operator, e.g. "+", "*", "max", "min"
};

class OmpCodegen {
public:
    // -----------------------------------------------------------------------
    // For every entry in `decisions`, look up the corresponding SgForStatement
    // via RoseBridge::loopMap[location], build the pragma text via
    // buildPragmaText(), and insert it as a SgPragmaDeclaration immediately
    // before that SgForStatement using SageInterface::insertStatementBefore.
    //
    // `ir` supplies the LoopNest data (array accesses, trip bound) needed to
    // build the pragma text; it is scanned per-function to find the LoopNest
    // matching each decision's location key.
    //
    // Locations present in `decisions` but absent from RoseBridge::loopMap
    // (or from `ir`) are silently skipped. Loops not present in `decisions`
    // are left completely untouched (REQ-C5).
    // -----------------------------------------------------------------------
    static void applyPragmas(SgProject* project,
                              const ProgramIR& ir,
                              const std::map<std::string, OmpDecision>& decisions);

    // -----------------------------------------------------------------------
    // Build the pragma directive TEXT for one loop, given its decision info.
    // The returned string does NOT include the leading "#pragma " token —
    // it is the directive body as expected by
    // SageBuilder::buildPragmaDeclaration(), e.g.:
    //   "omp target teams distribute parallel for map(to: a[0:N]) reduction(+:s)"
    //   "omp parallel for reduction(+:s)"
    // Exposed separately so it is independently testable without a full
    // SgProject (REQ-C1..C4).
    // -----------------------------------------------------------------------
    static std::string buildPragmaText(const LoopNest& loop, const OmpDecision& decision);
};

#pragma once
// ParallelismChecker.h — Task 5: loop parallelism classification
//
// Consumes the ClangBridge ProgramIR (LoopNest / ArrayAccess / CallSite) and
// the InterproceduralAnalysis summaries to classify every loop nest as one of
// PARALLEL_SAFE / REDUCTION_CANDIDATE / SERIAL_ONLY / UNKNOWN_CONSERVATIVE,
// per PRD §6.4 (REQ-P1..REQ-P5).
//
// ── Design note / deviation from the PRD's literal wording ─────────────────
// REQ-P1 says to "run DepInfoAnal on all array accesses ... to detect
// loop-carried dependences." DepInfoAnal (ROSE's StmtDepAnal/DepInfoAnal)
// operates on a reconstructed SgProject AST, which is not guaranteed to be
// available (RoseBridge may not have run, or may have failed and fallen
// back). It is also unverified against this ROSE build's actual API surface
// in this isolated task, so wiring it in here would be a compile-time risk
// this module cannot afford — it must build standalone with only
// ClangBridge.h + InterproceduralAnalysis.h.
//
// Instead, this checker implements REQ-P1 as a *textual/affine* dependence
// test directly over ArrayAccess.indexExpr strings extracted by the Clang
// bridge, which is always available. This is a legitimate, PRD-sanctioned
// simplification ("adapt reasonably... the IR-only path must always work
// standalone"). Only the simplest single-variable-plus-constant-offset
// affine form (e.g. "i", "i+1", "i - 2") relative to the loop's
// inductionVar is understood; anything else (a different variable, a
// product like "i*2+j", a function call, a bare numeric constant, etc.)
// is treated conservatively as UNKNOWN_CONSERVATIVE per REQ-P4, since we
// cannot prove it either safe or unsafe from text alone.
//
// A SgProject* hook is intentionally *not* exposed on classify() to keep the
// required signature stable and the IR-only path unconditionally available;
// see the note above for why the ROSE DepInfoAnal path was left out rather
// than added half-verified.
//
// ── REQ-P5 (reduction detection) limitation ─────────────────────────────
// ArrayAccess only records *array subscript* accesses. A classic scalar
// reduction ("sum += A[i]") touches a plain scalar variable ("sum") that
// never appears as an ArrayAccess at all — the Clang bridge IR simply does
// not carry enough information to see it. We therefore cannot reliably
// detect the general case from this IR shape. What we implement is a
// best-effort, low-false-positive heuristic: a loop is flagged
// REDUCTION_CANDIDATE only when it contains a call site whose callee name
// looks like a reduction/accumulation helper (e.g. contains "accumulate" or
// "reduce", case-insensitive) AND the loop is otherwise PARALLEL_SAFE. This
// intentionally under-detects (many real reductions will fall through to
// PARALLEL_SAFE or SERIAL_ONLY depending on the scalar's true dependence
// pattern, which we cannot observe) rather than over-claim a reduction we
// can't justify. This is called out explicitly because REQ-P1–P4 (safety
// classification) are the important, testable part of this task.

#include "ClangBridge.h"
#include "InterproceduralAnalysis.h"

#include <map>
#include <ostream>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// LoopClassification — REQ-P3
// ---------------------------------------------------------------------------
enum LoopClassification {
    PARALLEL_SAFE,
    REDUCTION_CANDIDATE,
    SERIAL_ONLY,
    UNKNOWN_CONSERVATIVE
};

// Human-readable name for a classification (used by --check-parallel output
// and logging).
std::string toString(LoopClassification c);

// ---------------------------------------------------------------------------
// LoopVerdict — classification result for a single loop nest.
// reductionVar / reductionOp are only meaningful when
// classification == REDUCTION_CANDIDATE (REQ-P5).
// ---------------------------------------------------------------------------
struct LoopVerdict {
    LoopClassification classification = UNKNOWN_CONSERVATIVE;
    std::string reason;
    std::string reductionVar;  // e.g. "sum"; empty if not a reduction
    std::string reductionOp;   // e.g. "+"; empty if not a reduction
};

// ---------------------------------------------------------------------------
// ParallelismChecker
// ---------------------------------------------------------------------------
class ParallelismChecker {
public:
    // Classify a single loop nest. `enclosingFunc` is the FunctionIR that
    // contains `loop` (used to know the parameter list, so we can guess
    // whether an array accessed in the loop is a global vs. a parameter for
    // the REQ-P2 "modifiesGlobals" check). `interproc` supplies callee
    // mod/ref summaries.
    LoopVerdict classify(const LoopNest& loop,
                          const FunctionIR& enclosingFunc,
                          const InterproceduralAnalysis& interproc) const;

    // Classify every loop in every defined function of the whole program.
    // Keyed by function name; each vector entry corresponds (by index) to
    // the loop at the same index in FunctionIR::loops.
    std::map<std::string, std::vector<LoopVerdict>> classifyAll(
        const ProgramIR& ir,
        const InterproceduralAnalysis& interproc) const;

    // Convenience driver for the `--check-parallel` CLI mode: classifies
    // every loop and prints one line per loop in the form
    //   "<location>: <CLASSIFICATION> (<reason>)"
    // to `out`.
    void printReport(const ProgramIR& ir,
                      const InterproceduralAnalysis& interproc,
                      std::ostream& out) const;

private:
    // Result of parsing an ArrayAccess::indexExpr as "IV", "IV+K", "IV-K"
    // relative to a given induction variable name.
    struct AffineIndex {
        bool isAffine;
        long offset;  // meaningful only if isAffine
    };

    // Parse indexExpr against inductionVar using the regex `IV([+-]\d+)?`
    // (whitespace-tolerant, full-string match only). Returns isAffine=false
    // for anything else (different variable, multiplication, function call,
    // bare constant, etc.) per REQ-P4.
    static AffineIndex parseAffineIndex(const std::string& indexExpr,
                                         const std::string& inductionVar);

    // REQ-P1/REQ-P4: examine loop.arrayAccesses for loop-carried dependences
    // and non-affine index expressions. On a definite loop-carried
    // dependence, sets *serial=true and fills reasonOut. Otherwise, if any
    // array access was non-affine, sets *unknown=true and fills reasonOut.
    static void checkArrayDependences(const LoopNest& loop,
                                       bool& serial,
                                       bool& unknown,
                                       std::string& reasonOut);

    // REQ-P2: examine loop.callSites against interproc summaries. On a
    // definite unsafe callee effect, sets *serial=true and fills reasonOut.
    // Otherwise, if any callee summary was conservative/unresolved, sets
    // *unknown=true and fills reasonOut.
    static void checkCallSiteEffects(const LoopNest& loop,
                                      const FunctionIR& enclosingFunc,
                                      const InterproceduralAnalysis& interproc,
                                      bool& serial,
                                      bool& unknown,
                                      std::string& reasonOut);

    // REQ-P5 best-effort heuristic (see file-header note). Returns a verdict
    // with classification == REDUCTION_CANDIDATE only when confident;
    // otherwise returns classification == PARALLEL_SAFE as a "no reduction
    // detected" sentinel (caller should not treat that as authoritative on
    // its own — it's only consulted once the loop is already known safe).
    static LoopVerdict detectReduction(const LoopNest& loop);
};

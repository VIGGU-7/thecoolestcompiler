#pragma once
// InterproceduralAnalysis.h — Call-graph construction and bottom-up mod/ref
// propagation for the interprocedural GPU-offload parallelizing compiler.
//
// Task 4 of the thecoolestcompiler project.
//
// Key design points:
//   • Uses ROSE's CallGraphBuilder + LocalRWSetGenerator as primary path.
//   • Falls back to manual SgVarRefExp traversal if LocalRWSetGenerator fails.
//   • Falls back to pure ProgramIR analysis if SgProject is nullptr or
//     CallGraphBuilder throws.
//   • SCCs with >1 member are treated conservatively (isConservative=true).
//   • Extern/unresolved callees get conservative summaries unless they are in
//     the known-pure whitelist.

#include "ClangBridge.h"
#include "rose.h"
#include "CallGraph.h"
#include "LocalRWSetGenerator.h"
#include "ReadWriteSetRecords.h"
#include <unordered_set>
#include <unordered_map>
#include <ostream>
#include <string>
#include <vector>

// ─────────────────────────────────────────────────────────────────────────────
// VarAccess — one entry in a per-function read or write set
// ─────────────────────────────────────────────────────────────────────────────
struct VarAccess {
    std::string varName;
    ReadWriteSets::Globality globality;
    ReadWriteSets::VarType  varType;
    bool isWrite;  // true = mod (appears in write set), false = ref (read set)
};

// ─────────────────────────────────────────────────────────────────────────────
// FunctionSummary — interprocedural mod/ref summary for one function
// ─────────────────────────────────────────────────────────────────────────────
struct FunctionSummary {
    std::string funcName;

    std::unordered_set<std::string> modSet;  // variables modified (transitively)
    std::unordered_set<std::string> refSet;  // variables referenced (transitively)

    bool modifiesGlobals;   // true if any global/file-scope var is in modSet
    bool isConservative;    // true if summary is over-approximated
    bool isPure;            // true if no side effects at all
};

// ─────────────────────────────────────────────────────────────────────────────
// InterproceduralAnalysis
// ─────────────────────────────────────────────────────────────────────────────
class InterproceduralAnalysis {
public:
    // ── Main entry point ────────────────────────────────────────────────────

    /// Build the call graph (if project != nullptr) and propagate mod/ref
    /// bottom-up.  Falls back to pure-IR analysis when ROSE analysis fails.
    void analyze(SgProject* project, const ProgramIR& ir);

    // ── Query interface ──────────────────────────────────────────────────────

    /// Return the summary for funcName. Returns a conservative default if no
    /// summary has been computed (e.g., unresolved extern).
    const FunctionSummary& getSummary(const std::string& funcName) const;

    /// True iff the named callee's transitively-computed modSet overlaps with
    /// any element of varSet.
    bool calleeModifiesAny(const std::string& callee,
                           const std::unordered_set<std::string>& varSet) const;

    /// Print all summaries to out (used by --dump-modref CLI flag).
    void dump(std::ostream& out) const;

private:
    // ── Internal state ───────────────────────────────────────────────────────

    std::unordered_map<std::string, FunctionSummary> summaries_;

    /// Fallback summary returned for unknown functions (always conservative).
    mutable FunctionSummary unknownSummary_;

    // Call-graph representation built from ROSE (caller → list-of-callees).
    // Stored as name→name to be independent of SgNode pointers after analysis.
    std::unordered_map<std::string, std::vector<std::string>> callGraph_;

    // ── Known-pure function whitelist ────────────────────────────────────────
    // These functions are treated as having no effect on user variables.
    // printf/fprintf write to streams, not user data — still not "pure" in the
    // strictest sense, but they do not modify user-visible program variables.
    static const std::unordered_set<std::string> pureFunctions_;

    // ── Analysis phases ──────────────────────────────────────────────────────

    /// Phase 1: populate callGraph_ from ROSE's CallGraphBuilder.
    /// Throws if the project has no translation units; caller catches.
    void buildCallGraph(SgProject* project);

    /// Phase 2: use LocalRWSetGenerator (or manual fallback) to populate
    /// summaries_ with local (intra-procedural) mod/ref sets.
    void computeLocalSummaries(SgProject* project);

    /// Phase 3: propagate summaries bottom-up through the call graph using a
    /// Tarjan SCC + reverse-topological-order worklist algorithm.
    void propagateBottomUp();

    /// Ensure an entry exists for a callee not found in the project.
    void handleUnknownCallee(const std::string& name);

    // ── Fallback IR-only analysis ────────────────────────────────────────────

    /// Populate summaries_ from the ClangBridge ProgramIR alone (no ROSE).
    void analyzeFromIR(const ProgramIR& ir);

    // ── Tarjan SCC helpers ───────────────────────────────────────────────────

    /// Compute SCCs using Tarjan's algorithm on callGraph_.
    /// Returns a list of SCCs ordered in reverse topological order (leaves
    /// first, i.e., callees before callers — ready for bottom-up processing).
    std::vector<std::vector<std::string>> computeSCCs() const;

    // Tarjan DFS state
    struct TarjanState {
        std::unordered_map<std::string, int>  index;
        std::unordered_map<std::string, int>  lowlink;
        std::unordered_map<std::string, bool> onStack;
        std::vector<std::string>              stack;
        std::vector<std::vector<std::string>> sccs;
        int                                   nextIndex = 0;
    };

    void tarjanDFS(const std::string& node, TarjanState& state) const;

    // ── Utility helpers ──────────────────────────────────────────────────────

    /// Merge all fields from src into dst. Returns true if dst changed.
    static bool mergeSummary(FunctionSummary& dst, const FunctionSummary& src);

    /// True if the variable name looks like a global (no '@' prefix used by
    /// LocalRWSetGenerator to denote locals).
    static bool isGlobalName(const std::string& varName);

    /// Extract variable name string from an AccessSetRecord (strips ROSE
    /// LocalRWSetGenerator decoration).
    static std::string extractVarName(const ReadWriteSets::AccessSetRecord& rec);
};

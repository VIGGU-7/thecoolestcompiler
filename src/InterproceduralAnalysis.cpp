// InterproceduralAnalysis.cpp — Implementation of call-graph construction and
// bottom-up mod/ref propagation.
//
// Analysis pipeline (in analyze()):
//   1. buildCallGraph()       — ROSE CallGraphBuilder → callGraph_ map
//   2. computeLocalSummaries()— LocalRWSetGenerator (or manual fallback)
//   3. propagateBottomUp()    — Tarjan SCCs + reverse-topo worklist
//
// If the SgProject is nullptr or ROSE throws at any stage, we fall back to
// analyzeFromIR() which uses the ClangBridge ProgramIR exclusively.
//
// Task 4 — thecoolestcompiler

#include "InterproceduralAnalysis.h"
#include "sageInterface.h"   // SageInterface::querySubTree etc.
#include "sageGeneric.h"     // isSg* helpers

#include <algorithm>
#include <iostream>
#include <stdexcept>

// ─────────────────────────────────────────────────────────────────────────────
// Static data
// ─────────────────────────────────────────────────────────────────────────────

// Known-pure function whitelist (REQ-I4).
// Functions here are treated as having no effect on user-visible program
// variables.  printf/fprintf write to FILE streams — not to user variables —
// so we include them here (they do NOT modify any user variable), but we do
// NOT mark them isPure=true (they have side effects on the stream).
const std::unordered_set<std::string> InterproceduralAnalysis::pureFunctions_ = {
    "sqrt",   "sqrtf",  "sqrtl",
    "fabs",   "fabsf",  "fabsl",
    "sin",    "sinf",   "sinl",
    "cos",    "cosf",   "cosl",
    "exp",    "expf",   "expl",
    "log",    "logf",   "logl",
    "pow",    "powf",   "powl",
    "abs",    "llabs",
    "memcpy", "memmove",
    "strlen",
    "printf", "fprintf",  // stream-only side effects
};

// ─────────────────────────────────────────────────────────────────────────────
// analyze() — main entry point
// ─────────────────────────────────────────────────────────────────────────────

void InterproceduralAnalysis::analyze(SgProject* project, const ProgramIR& ir) {
    // Initialize the unknown-function fallback summary once.
    unknownSummary_.funcName        = "<unknown>";
    unknownSummary_.modifiesGlobals = true;
    unknownSummary_.isConservative  = true;
    unknownSummary_.isPure          = false;

    if (project == nullptr) {
        // No ROSE AST available — fall back to ClangBridge IR only.
        std::cerr << "[InterproceduralAnalysis] SgProject is nullptr, "
                     "using IR-only analysis.\n";
        analyzeFromIR(ir);
        return;
    }

    // ── Phase 1: Build call graph ────────────────────────────────────────────
    bool callGraphOk = false;
    try {
        buildCallGraph(project);
        callGraphOk = true;
    } catch (const std::exception& ex) {
        std::cerr << "[InterproceduralAnalysis] CallGraphBuilder failed ("
                  << ex.what() << "), falling back to IR-only analysis.\n";
    } catch (...) {
        std::cerr << "[InterproceduralAnalysis] CallGraphBuilder threw unknown "
                     "exception, falling back to IR-only analysis.\n";
    }

    if (!callGraphOk) {
        analyzeFromIR(ir);
        return;
    }

    // Augment call graph with edges from the ClangBridge IR (cross-check /
    // fill gaps — REQ-I3 asks for a parallel cross-check).
    for (const auto& kv : ir) {
        const FunctionIR& fir = kv.second;
        if (callGraph_.find(fir.name) == callGraph_.end()) {
            callGraph_[fir.name] = {};
        }
        for (const CallSite& cs : fir.calls) {
            auto& callees = callGraph_[fir.name];
            if (std::find(callees.begin(), callees.end(), cs.calleeName) ==
                callees.end()) {
                callees.push_back(cs.calleeName);
            }
        }
    }

    // NOTE: an earlier version of this function pre-seeded an empty summary
    // here for every ClangBridge-defined function, to fix a real key-format
    // mismatch bug (ROSE keys summaries as "::name(double)", ClangBridge/
    // ParallelismChecker look them up as plain "name", so real functions
    // were falling through to the conservative "unknown callee" path).
    // That fix was REVERTED: it silently flipped functions with genuine but
    // ClangBridge-invisible side effects (e.g. a plain global scalar write
    // like `running_total += value;` — ArrayAccess only records array
    // subscripts, never scalar variable reads/writes, so this is completely
    // invisible to the current IR) from correctly-conservative to
    // incorrectly-"pure", which would have let ParallelismChecker approve a
    // genuinely order-dependent loop for parallelization. A false SAFE
    // verdict is a correctness bug in a parallelizing compiler; a false
    // CONSERVATIVE verdict just leaves performance on the table. Reverting
    // trades a known missed-optimization (real, side-effect-free callees
    // like a pure math helper don't get proven safe) for soundness, which
    // is the right tradeoff until ClangBridge is extended to track scalar
    // (non-subscript) variable accesses and global declarations too — see
    // CONTEXT.md "Known remaining gaps".

    // ── Phase 2: Compute local summaries ────────────────────────────────────
    try {
        computeLocalSummaries(project);
    } catch (const std::exception& ex) {
        std::cerr << "[InterproceduralAnalysis] computeLocalSummaries failed ("
                  << ex.what() << "), summaries may be incomplete.\n";
    } catch (...) {
        std::cerr << "[InterproceduralAnalysis] computeLocalSummaries threw "
                     "unknown exception.\n";
    }

    // Seed summaries for any functions visible in the IR but missing from ROSE.
    for (const auto& kv : ir) {
        const FunctionIR& fir = kv.second;
        if (summaries_.find(fir.name) == summaries_.end()) {
            if (!fir.hasDefinition) {
                handleUnknownCallee(fir.name);
            } else {
                // Defined in IR but not processed by ROSE — create empty entry.
                FunctionSummary& s = summaries_[fir.name];
                s.funcName         = fir.name;
                s.modifiesGlobals  = false;
                s.isConservative   = false;
                s.isPure           = false;
            }
        }
    }

    // ── Phase 3: Bottom-up propagation ──────────────────────────────────────
    propagateBottomUp();
}

// ─────────────────────────────────────────────────────────────────────────────
// buildCallGraph() — Phase 1
// ─────────────────────────────────────────────────────────────────────────────

void InterproceduralAnalysis::buildCallGraph(SgProject* project) {
    // CallGraphBuilder requires at least one input file.
    if (project->get_fileList().empty()) {
        throw std::runtime_error("SgProject has no files");
    }

    CallGraphBuilder cgBuilder(project);

    // builtinFilter() filters out compiler-internal __builtin_* functions.
    cgBuilder.buildCallGraph(builtinFilter());

    SgIncidenceDirectedGraph* cg = cgBuilder.getGraph();
    if (cg == nullptr) {
        throw std::runtime_error("CallGraphBuilder returned null graph");
    }

    // ── Extract caller→callees from the graph ────────────────────────────────
    // The ROSE call graph stores edges as caller→callee (directed).
    // We iterate all graph nodes; for each node we get its successors (callees).

    const auto& nodeMap = cg->get_node_index_to_node_map();
    for (const auto& kv : nodeMap) {
        SgGraphNode* callerNode = isSgGraphNode(kv.second);
        if (callerNode == nullptr) continue;

        SgNode* callerSg = callerNode->get_SgNode();
        SgFunctionDeclaration* callerDecl = isSgFunctionDeclaration(callerSg);
        if (callerDecl == nullptr) continue;

        std::string callerName = callerDecl->get_qualified_name().getString();
        if (callGraph_.find(callerName) == callGraph_.end()) {
            callGraph_[callerName] = {};
        }

        // Get successors (callees) of this node.
        std::vector<SgGraphNode*> successors;
        cg->getSuccessors(callerNode, successors);

        for (SgGraphNode* calleeNode : successors) {
            SgNode* calleeSg = calleeNode->get_SgNode();
            SgFunctionDeclaration* calleeDecl = isSgFunctionDeclaration(calleeSg);
            if (calleeDecl == nullptr) continue;

            std::string calleeName = calleeDecl->get_qualified_name().getString();
            auto& callees = callGraph_[callerName];
            if (std::find(callees.begin(), callees.end(), calleeName) ==
                callees.end()) {
                callees.push_back(calleeName);
            }

            // Ensure callee has an entry too.
            if (callGraph_.find(calleeName) == callGraph_.end()) {
                callGraph_[calleeName] = {};
            }
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// computeLocalSummaries() — Phase 2
// ─────────────────────────────────────────────────────────────────────────────

void InterproceduralAnalysis::computeLocalSummaries(SgProject* project) {
    // ── Primary path: LocalRWSetGenerator ───────────────────────────────────
    // Try to use LocalRWSetGenerator. We wrap each per-function call in its
    // own try/catch so a failure on one function does not abort the whole pass.

    bool rwGeneratorSucceeded = false;

    try {
        // collectReadWriteSets populates an internal cache over all functions.
        std::string cmdLine = "thecoolestcompiler";
        LocalRWSetGenerator rwGen(cmdLine);
        rwGen.collectReadWriteSets(project);

        const auto& cache = rwGen.getRWSetCache();
        for (const ReadWriteSets::FunctionReadWriteRecord& rec : cache) {
            // internalFunctionName has the form "::<qualified>::name".
            std::string funcName = rec.internalFunctionName;

            FunctionSummary& s = summaries_[funcName];
            s.funcName         = funcName;
            s.isConservative   = false;
            s.modifiesGlobals  = false;

            // ── Write set → modSet ─────────────────────────────────────────
            for (const ReadWriteSets::AccessSetRecord& ar : rec.writeSet) {
                std::string vname = extractVarName(ar);
                s.modSet.insert(vname);
                if (ar.globality >= ReadWriteSets::FILE_SCOPE) {
                    s.modifiesGlobals = true;
                }
            }

            // ── Read set → refSet ──────────────────────────────────────────
            for (const ReadWriteSets::AccessSetRecord& ar : rec.readSet) {
                std::string vname = extractVarName(ar);
                s.refSet.insert(vname);
            }

            s.isPure = s.modSet.empty() && s.refSet.empty();
        }

        rwGeneratorSucceeded = true;

    } catch (const std::exception& ex) {
        std::cerr << "[InterproceduralAnalysis] LocalRWSetGenerator failed ("
                  << ex.what() << "), falling back to SgVarRefExp traversal.\n";
    } catch (...) {
        std::cerr << "[InterproceduralAnalysis] LocalRWSetGenerator threw "
                     "unknown exception, falling back to SgVarRefExp traversal.\n";
    }

    if (rwGeneratorSucceeded) {
        // Also make sure all functions seen in the call graph have entries,
        // even if LocalRWSetGenerator did not produce a record for them.
        for (const auto& kv : callGraph_) {
            if (summaries_.find(kv.first) == summaries_.end()) {
                handleUnknownCallee(kv.first);
            }
        }
        return;
    }

    // ── Fallback path: manual SgVarRefExp traversal ─────────────────────────
    // Walk every function definition in the project. For each SgVarRefExp,
    // determine if it is a write (LHS of assignment) or a read, and determine
    // the variable's scope to classify its globality.

    // Collect all function definitions.
    Rose_STL_Container<SgNode*> funcDefs =
        NodeQuery::querySubTree(project, V_SgFunctionDefinition);

    for (SgNode* node : funcDefs) {
        SgFunctionDefinition* funcDef = isSgFunctionDefinition(node);
        if (funcDef == nullptr) continue;

        SgFunctionDeclaration* funcDecl = funcDef->get_declaration();
        if (funcDecl == nullptr) continue;

        std::string funcName = funcDecl->get_qualified_name().getString();

        FunctionSummary& s = summaries_[funcName];
        s.funcName         = funcName;
        s.isConservative   = false;
        s.modifiesGlobals  = false;

        // Collect all variable references in this function.
        Rose_STL_Container<SgNode*> varRefs =
            NodeQuery::querySubTree(funcDef, V_SgVarRefExp);

        for (SgNode* vn : varRefs) {
            SgVarRefExp* varRef = isSgVarRefExp(vn);
            if (varRef == nullptr) continue;

            SgInitializedName* initName = varRef->get_symbol()->get_declaration();
            if (initName == nullptr) continue;

            std::string varName = initName->get_qualified_name().getString();
            if (varName.empty()) varName = initName->get_name().getString();

            // Determine if this is a global/file-scope variable.
            bool isGlobal = false;
            SgScopeStatement* declScope = initName->get_scope();
            if (declScope != nullptr) {
                isGlobal = (isSgGlobal(declScope) != nullptr) ||
                           (isSgNamespaceDefinitionStatement(declScope) != nullptr);
            }

            // Determine write vs. read by checking parent context.
            bool isWrite = false;
            SgNode* parent = varRef->get_parent();
            if (SgAssignOp* assignOp = isSgAssignOp(parent)) {
                // Write if we are the LHS of =
                isWrite = (assignOp->get_lhs_operand() == varRef);
            } else if (isSgPlusAssignOp(parent)  || isSgMinusAssignOp(parent) ||
                       isSgMultAssignOp(parent)   || isSgDivAssignOp(parent)   ||
                       isSgModAssignOp(parent)     ||
                       isSgPlusPlusOp(parent)      || isSgMinusMinusOp(parent)) {
                isWrite = true;
            } else if (SgPntrArrRefExp* arrRef = isSgPntrArrRefExp(parent)) {
                // Array element write if the array is on the LHS of assignment.
                SgNode* arrParent = arrRef->get_parent();
                if (SgAssignOp* ap = isSgAssignOp(arrParent)) {
                    isWrite = (ap->get_lhs_operand() == arrRef);
                }
            }

            if (isWrite) {
                s.modSet.insert(varName);
                if (isGlobal) s.modifiesGlobals = true;
            } else {
                s.refSet.insert(varName);
            }
        }

        s.isPure = s.modSet.empty() && s.refSet.empty();
    }

    // Handle callees seen in the graph but not in function definitions.
    for (const auto& kv : callGraph_) {
        if (summaries_.find(kv.first) == summaries_.end()) {
            handleUnknownCallee(kv.first);
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// propagateBottomUp() — Phase 3
// ─────────────────────────────────────────────────────────────────────────────

void InterproceduralAnalysis::propagateBottomUp() {
    if (callGraph_.empty()) return;

    // ── Step 1: Compute SCCs in reverse topological order ───────────────────
    // computeSCCs() returns SCCs ordered so that callees come before callers
    // (leaves-first / reverse topological), ready for bottom-up propagation.
    std::vector<std::vector<std::string>> sccs = computeSCCs();

    // ── Step 2: Process each SCC ─────────────────────────────────────────────
    bool changed = true;
    while (changed) {
        changed = false;

        for (const std::vector<std::string>& scc : sccs) {
            // ── Cycles: union all members and mark conservative ───────────
            if (scc.size() > 1) {
                // Build merged summary for the SCC.
                FunctionSummary merged;
                merged.funcName       = "<SCC>";
                merged.isConservative = true;
                merged.modifiesGlobals = false;
                merged.isPure         = false;

                for (const std::string& fname : scc) {
                    auto it = summaries_.find(fname);
                    if (it != summaries_.end()) {
                        const FunctionSummary& s = it->second;
                        merged.modSet.insert(s.modSet.begin(), s.modSet.end());
                        merged.refSet.insert(s.refSet.begin(), s.refSet.end());
                        if (s.modifiesGlobals) merged.modifiesGlobals = true;
                    }
                }

                // Distribute merged summary back to every SCC member.
                for (const std::string& fname : scc) {
                    FunctionSummary& s = summaries_[fname];
                    if (mergeSummary(s, merged)) changed = true;
                    s.isConservative = true;
                }
            }

            // ── Propagate each member's summary to its callers ────────────
            // In our callGraph_ structure: callerName → [calleeName, ...].
            // We need the reverse: for each member of this SCC, find all
            // callers and push this SCC's summary up.
            for (const std::string& sccMember : scc) {
                auto it = summaries_.find(sccMember);
                if (it == summaries_.end()) continue;
                const FunctionSummary& calleeSummary = it->second;

                // Walk the call graph to find callers of sccMember.
                for (auto& callerKv : callGraph_) {
                    const std::string& callerName = callerKv.first;
                    const std::vector<std::string>& callees = callerKv.second;

                    bool isCallee =
                        std::find(callees.begin(), callees.end(), sccMember) !=
                        callees.end();
                    if (!isCallee) continue;

                    // Do not propagate from a node into itself — that case is
                    // handled by the SCC-union above.
                    bool isSameSCC =
                        std::find(scc.begin(), scc.end(), callerName) != scc.end();
                    if (isSameSCC) continue;

                    FunctionSummary& callerSummary = summaries_[callerName];
                    if (callerSummary.funcName.empty())
                        callerSummary.funcName = callerName;

                    if (mergeSummary(callerSummary, calleeSummary)) changed = true;
                }
            }
        }
    }

    // ── Step 3: Re-compute isPure and modifiesGlobals after propagation ──────
    for (auto& kv : summaries_) {
        FunctionSummary& s = kv.second;
        // isPure: no side effects at all.
        s.isPure = s.modSet.empty() && !s.modifiesGlobals;
        // Whitelist overrides.
        if (pureFunctions_.count(kv.first)) {
            s.isPure = true;
            s.modifiesGlobals = false;
            s.isConservative  = false;
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// handleUnknownCallee() — conservative entry for extern functions
// ─────────────────────────────────────────────────────────────────────────────

void InterproceduralAnalysis::handleUnknownCallee(const std::string& name) {
    FunctionSummary& s = summaries_[name];
    s.funcName         = name;

    if (pureFunctions_.count(name)) {
        // Known-pure: no user-variable side effects.
        s.isPure          = true;
        s.modifiesGlobals = false;
        s.isConservative  = false;
    } else {
        // Unknown extern: assume it may modify anything global (REQ-I4).
        s.modifiesGlobals = true;
        s.isConservative  = true;
        s.isPure          = false;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// analyzeFromIR() — ClangBridge ProgramIR fallback
// ─────────────────────────────────────────────────────────────────────────────

void InterproceduralAnalysis::analyzeFromIR(const ProgramIR& ir) {
    // ── Step 1: Build call graph from IR ────────────────────────────────────
    for (const auto& kv : ir) {
        const FunctionIR& fir = kv.second;
        callGraph_[fir.name] = {};
        for (const CallSite& cs : fir.calls) {
            callGraph_[fir.name].push_back(cs.calleeName);
            if (callGraph_.find(cs.calleeName) == callGraph_.end()) {
                callGraph_[cs.calleeName] = {};
            }
        }
    }

    // ── Step 2: Seed local summaries from IR loop accesses ──────────────────
    for (const auto& kv : ir) {
        const FunctionIR& fir = kv.second;

        if (!fir.hasDefinition) {
            handleUnknownCallee(fir.name);
            continue;
        }

        FunctionSummary& s = summaries_[fir.name];
        s.funcName         = fir.name;
        s.modifiesGlobals  = false;
        s.isConservative   = false;
        s.isPure           = false;

        // Use the loop accesses extracted by the Clang bridge.
        for (const LoopNest& loop : fir.loops) {
            for (const ArrayAccess& acc : loop.arrayAccesses) {
                if (acc.isWrite) {
                    s.modSet.insert(acc.arrayName);
                } else {
                    s.refSet.insert(acc.arrayName);
                }
            }
        }

        s.isPure = s.modSet.empty() && s.refSet.empty();
    }

    // Seed unknown callees referenced in the IR.
    for (const auto& kv : callGraph_) {
        if (summaries_.find(kv.first) == summaries_.end()) {
            handleUnknownCallee(kv.first);
        }
    }

    // ── Step 3: Propagate bottom-up ─────────────────────────────────────────
    propagateBottomUp();
}

// ─────────────────────────────────────────────────────────────────────────────
// getSummary() — query interface
// ─────────────────────────────────────────────────────────────────────────────

const FunctionSummary& InterproceduralAnalysis::getSummary(
    const std::string& funcName) const {
    auto it = summaries_.find(funcName);
    if (it != summaries_.end()) return it->second;

    // Return a conservative default for any function not yet summarised.
    unknownSummary_.funcName = funcName;
    return unknownSummary_;
}

// ─────────────────────────────────────────────────────────────────────────────
// calleeModifiesAny()
// ─────────────────────────────────────────────────────────────────────────────

bool InterproceduralAnalysis::calleeModifiesAny(
    const std::string& callee,
    const std::unordered_set<std::string>& varSet) const {

    auto it = summaries_.find(callee);
    if (it == summaries_.end()) {
        // Unknown callee — conservatively say "yes, it modifies something".
        return true;
    }

    const FunctionSummary& s = it->second;

    // Conservative summary: assume it modifies everything.
    if (s.isConservative && s.modifiesGlobals) return true;

    // Check if any variable in varSet appears in the callee's mod set.
    for (const std::string& var : varSet) {
        if (s.modSet.count(var)) return true;
    }

    return false;
}

// ─────────────────────────────────────────────────────────────────────────────
// dump()
// ─────────────────────────────────────────────────────────────────────────────

void InterproceduralAnalysis::dump(std::ostream& out) const {
    out << "=== Interprocedural Mod/Ref Summaries ===\n";
    out << "Total functions summarised: " << summaries_.size() << "\n\n";

    // Sort by function name for deterministic output.
    std::vector<std::string> names;
    names.reserve(summaries_.size());
    for (const auto& kv : summaries_) names.push_back(kv.first);
    std::sort(names.begin(), names.end());

    for (const std::string& name : names) {
        const FunctionSummary& s = summaries_.at(name);
        out << "Function: " << s.funcName << "\n";
        out << "  isPure:           " << (s.isPure          ? "true" : "false") << "\n";
        out << "  modifiesGlobals:  " << (s.modifiesGlobals ? "true" : "false") << "\n";
        out << "  isConservative:   " << (s.isConservative  ? "true" : "false") << "\n";

        out << "  modSet (" << s.modSet.size() << "): ";
        {
            std::vector<std::string> sorted(s.modSet.begin(), s.modSet.end());
            std::sort(sorted.begin(), sorted.end());
            for (const std::string& v : sorted) out << v << " ";
        }
        out << "\n";

        out << "  refSet (" << s.refSet.size() << "): ";
        {
            std::vector<std::string> sorted(s.refSet.begin(), s.refSet.end());
            std::sort(sorted.begin(), sorted.end());
            for (const std::string& v : sorted) out << v << " ";
        }
        out << "\n\n";
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Tarjan SCC implementation
// ─────────────────────────────────────────────────────────────────────────────

std::vector<std::vector<std::string>>
InterproceduralAnalysis::computeSCCs() const {
    TarjanState state;

    for (const auto& kv : callGraph_) {
        if (state.index.find(kv.first) == state.index.end()) {
            tarjanDFS(kv.first, state);
        }
    }

    // Tarjan produces SCCs in reverse topological order (leaf SCCs first).
    // That is exactly what bottom-up propagation needs.
    return state.sccs;
}

void InterproceduralAnalysis::tarjanDFS(const std::string& node,
                                        TarjanState& state) const {
    state.index[node]   = state.nextIndex;
    state.lowlink[node] = state.nextIndex;
    state.nextIndex++;
    state.stack.push_back(node);
    state.onStack[node] = true;

    // Visit successors (callees).
    auto cgIt = callGraph_.find(node);
    if (cgIt != callGraph_.end()) {
        for (const std::string& callee : cgIt->second) {
            if (state.index.find(callee) == state.index.end()) {
                // Callee not yet visited — recurse.
                tarjanDFS(callee, state);
                state.lowlink[node] =
                    std::min(state.lowlink[node], state.lowlink[callee]);
            } else if (state.onStack[callee]) {
                // Back edge — callee is in the current SCC.
                state.lowlink[node] =
                    std::min(state.lowlink[node], state.index[callee]);
            }
            // If callee is finished (not onStack) — cross/forward edge, ignore.
        }
    }

    // If node is a root of an SCC, pop the SCC.
    if (state.lowlink[node] == state.index[node]) {
        std::vector<std::string> scc;
        while (true) {
            std::string w = state.stack.back();
            state.stack.pop_back();
            state.onStack[w] = false;
            scc.push_back(w);
            if (w == node) break;
        }
        state.sccs.push_back(std::move(scc));
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Utility helpers
// ─────────────────────────────────────────────────────────────────────────────

bool InterproceduralAnalysis::mergeSummary(FunctionSummary& dst,
                                            const FunctionSummary& src) {
    bool changed = false;

    for (const std::string& v : src.modSet) {
        if (dst.modSet.insert(v).second) changed = true;
    }
    for (const std::string& v : src.refSet) {
        if (dst.refSet.insert(v).second) changed = true;
    }
    if (src.modifiesGlobals && !dst.modifiesGlobals) {
        dst.modifiesGlobals = true;
        changed = true;
    }
    if (src.isConservative && !dst.isConservative) {
        dst.isConservative = true;
        changed = true;
    }
    return changed;
}

bool InterproceduralAnalysis::isGlobalName(const std::string& varName) {
    // LocalRWSetGenerator decorates locals with '@funcName@' prefix.
    // A name that does NOT contain '@' at position 0 is treated as global.
    return varName.empty() || varName[0] != '@';
}

std::string InterproceduralAnalysis::extractVarName(
    const ReadWriteSets::AccessSetRecord& rec) {
    // Use the variableName field (human-readable, as documented in header).
    const std::string& vn = rec.variableName;
    if (!vn.empty()) return vn;
    // Fall back to nodeId if variableName is empty (should rarely happen).
    return rec.nodeId;
}

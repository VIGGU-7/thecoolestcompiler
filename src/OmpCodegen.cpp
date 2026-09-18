// OmpCodegen.cpp — OpenMP pragma text generation and AST insertion (Task 8)
//
// See OmpCodegen.h for the design rationale. Implementation notes:
//
//   * SageBuilder::buildPragmaDeclaration(name, scope) expects `name` to be
//     the pragma directive body WITHOUT the literal "#pragma" prefix — i.e.
//     text starting with "omp ...". The unparser prints "#pragma " + name
//     itself. This mirrors standard ROSE usage (confirmed against the
//     sageBuilder.h signature: it takes a single `name` string used verbatim
//     as the SgPragma's text).
//
//   * We reuse the exact SageBuilder/SageInterface call style already proven
//     to compile in RoseBridge.cpp: `using namespace SageBuilder;` /
//     `using namespace SageInterface;`, then bare function calls.

#include "OmpCodegen.h"

#include "sageBuilder.h"
#include "sageInterface.h"

#include <set>
#include <sstream>

using namespace SageBuilder;
using namespace SageInterface;

namespace {

// ---------------------------------------------------------------------------
// Classify each distinct array name in loop.arrayAccesses into read-only,
// write-only, or read-write buckets (REQ-C1). Order of first appearance is
// preserved for deterministic, testable output.
// ---------------------------------------------------------------------------
struct ArrayClassification {
    std::vector<std::string> readOnly;
    std::vector<std::string> writeOnly;
    std::vector<std::string> readWrite;
};

ArrayClassification classifyArrays(const LoopNest& loop)
{
    ArrayClassification result;

    std::vector<std::string> order;          // first-seen order of array names
    std::map<std::string, bool> everRead;
    std::map<std::string, bool> everWritten;
    std::set<std::string> seen;

    for (const ArrayAccess& acc : loop.arrayAccesses) {
        if (acc.arrayName.empty()) continue;
        if (seen.insert(acc.arrayName).second) {
            order.push_back(acc.arrayName);
            everRead[acc.arrayName]    = false;
            everWritten[acc.arrayName] = false;
        }
        if (acc.isWrite) everWritten[acc.arrayName] = true;
        else             everRead[acc.arrayName]    = true;
    }

    for (const std::string& name : order) {
        bool r = everRead[name];
        bool w = everWritten[name];
        if (r && w)       result.readWrite.push_back(name);
        else if (w)       result.writeOnly.push_back(name);
        else               result.readOnly.push_back(name);
    }

    return result;
}

// ---------------------------------------------------------------------------
// Render a map(<kind>: a[0:N], b[0:N]) clause fragment. Returns "" if the
// array list is empty (caller omits the whole clause in that case).
// ---------------------------------------------------------------------------
std::string renderMapClause(const std::string& kind,
                            const std::vector<std::string>& arrays,
                            const std::string& tripBound)
{
    if (arrays.empty()) return "";

    std::ostringstream oss;
    oss << "map(" << kind << ": ";
    for (size_t i = 0; i < arrays.size(); ++i) {
        if (i > 0) oss << ", ";
        oss << arrays[i] << "[0:" << tripBound << "]";
    }
    oss << ")";
    return oss.str();
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// OmpCodegen::buildPragmaText
// ---------------------------------------------------------------------------
std::string OmpCodegen::buildPragmaText(const LoopNest& loop, const OmpDecision& decision)
{
    // REQ-C4: use the Clang-extracted trip bound as N in arr[0:N]. Fall back
    // to a conservative placeholder if it's missing (should not normally
    // happen — ClangBridge always populates tripBound).
    std::string N = loop.tripBound.empty() ? "N" : loop.tripBound;

    std::ostringstream pragma;

    if (decision.gpuProfitable) {
        // REQ-C1
        pragma << "omp target teams distribute parallel for";

        ArrayClassification cls = classifyArrays(loop);
        std::string toClause     = renderMapClause("to",     cls.readOnly,  N);
        std::string fromClause   = renderMapClause("from",   cls.writeOnly, N);
        std::string tofromClause = renderMapClause("tofrom", cls.readWrite, N);

        // Omit the whole map(...) section entirely if there are no arrays
        // at all (e.g. a pure scalar loop).
        if (!toClause.empty())     pragma << " " << toClause;
        if (!fromClause.empty())   pragma << " " << fromClause;
        if (!tofromClause.empty()) pragma << " " << tofromClause;
    } else {
        // REQ-C3
        pragma << "omp parallel for";
    }

    // REQ-C2 / REQ-C3: reduction clause applies to both GPU and CPU paths.
    if (decision.hasReduction && !decision.reductionVar.empty() && !decision.reductionOp.empty()) {
        pragma << " reduction(" << decision.reductionOp << ":" << decision.reductionVar << ")";
    }

    return pragma.str();
}

// ---------------------------------------------------------------------------
// OmpCodegen::applyPragmas
// ---------------------------------------------------------------------------
void OmpCodegen::applyPragmas(SgProject* /*project*/,
                              const ProgramIR& ir,
                              const std::map<std::string, OmpDecision>& decisions)
{
    for (const auto& kv : decisions) {
        const std::string& location = kv.first;
        const OmpDecision& decision = kv.second;

        // Resolve location -> SgForStatement via RoseBridge::loopMap.
        auto loopMapIt = RoseBridge::loopMap.find(location);
        if (loopMapIt == RoseBridge::loopMap.end() || loopMapIt->second == nullptr) {
            // Unknown/unbuilt loop — nothing to transform.
            continue;
        }
        SgForStatement* forStmt = loopMapIt->second;

        // Find the matching LoopNest (for arrayAccesses / tripBound) by
        // scanning the ProgramIR for a loop whose location matches.
        const LoopNest* matchedLoop = nullptr;
        for (const auto& fnKv : ir) {
            const FunctionIR& fn = fnKv.second;
            for (const LoopNest& loop : fn.loops) {
                if (loop.location == location) {
                    matchedLoop = &loop;
                    break;
                }
            }
            if (matchedLoop) break;
        }
        if (!matchedLoop) {
            // No IR data for this location — cannot build a correct pragma.
            continue;
        }

        std::string pragmaText = buildPragmaText(*matchedLoop, decision);
        if (pragmaText.empty()) continue;

        // buildPragmaDeclaration wants the enclosing scope for symbol-table
        // bookkeeping; insertStatementBefore itself only needs the target
        // statement (it locates the containing statement list, e.g. the
        // SgBasicBlock that is the loop's parent, on its own).
        SgScopeStatement* scope = SageInterface::getEnclosingScope(forStmt);

        SgPragmaDeclaration* pragmaDecl =
            SageBuilder::buildPragmaDeclaration(pragmaText, scope);
        if (!pragmaDecl) continue;

        SageInterface::insertStatementBefore(forStmt, pragmaDecl);
    }
}

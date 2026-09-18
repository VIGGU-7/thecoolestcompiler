// ClangBridge.cpp — Extracts a ProgramIR from a C/C++ source file using
// Clang's libtooling / RecursiveASTVisitor.
//
// NO ROSE dependency (REQ-F4) — this file must never #include "rose.h" or
// anything under rose/, so that ClangBridge::extractIR() is testable in
// complete isolation from the ROSE installation.
//
// Implementation notes / simplifications (documented per task instructions):
//   * Array index expressions are recorded as raw textual source (via
//     clang::Lexer::getSourceText), not parsed into an affine model — that
//     is the job of the later analysis passes (Task 4), which only need
//     the text per REQ-F2/REQ-P4.
//   * "isWrite" is determined structurally: an ArraySubscriptExpr is a
//     write if it is the direct LHS of `=` or a compound-assignment
//     operator (`+=`, `-=`, ...), or the operand of `++`/`--`; everything
//     else (including the array/index sub-expressions of a write, e.g. the
//     `B` and `i-1` in `A[i-1] = B[i]`) is a read. This matches REQ-F2's
//     "read/write" classification without needing full alias/points-to
//     analysis.
//   * mangledName is produced via clang::ItaniumMangleContext on a
//     best-effort basis; if mangling doesn't apply (e.g. extern "C",
//     plain C translation units) it simply equals `name`.
//   * A "top-level for loop nest" (REQ-F2) is a `for` loop that is not
//     itself lexically nested inside another `for` loop. Its LoopNest
//     entry aggregates array accesses / call sites from the *entire* nest
//     (including any loops nested inside it), and `depth` records the
//     deepest nesting level reached.

#include "ClangBridge.h"

#include <clang/AST/ASTConsumer.h>
#include <clang/AST/ASTContext.h>
#include <clang/AST/Decl.h>
#include <clang/AST/Expr.h>
#include <clang/AST/Mangle.h>
#include <clang/AST/RecursiveASTVisitor.h>
#include <clang/AST/Stmt.h>
#include <clang/Basic/SourceLocation.h>
#include <clang/Basic/SourceManager.h>
#include <clang/Frontend/CompilerInstance.h>
#include <clang/Frontend/FrontendAction.h>
#include <clang/Lex/Lexer.h>
#include <clang/Tooling/CompilationDatabase.h>
#include <clang/Tooling/Tooling.h>
#include <llvm/ADT/StringRef.h>
#include <llvm/Support/raw_ostream.h>

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

namespace {

// ---------------------------------------------------------------------
// Small textual/source helpers
// ---------------------------------------------------------------------

std::string getSourceText(clang::SourceRange range,
                           const clang::SourceManager& sm,
                           const clang::LangOptions& langOpts) {
    if (range.isInvalid()) {
        return std::string();
    }
    clang::CharSourceRange csr = clang::CharSourceRange::getTokenRange(range);
    bool invalid = false;
    llvm::StringRef text = clang::Lexer::getSourceText(csr, sm, langOpts, &invalid);
    if (invalid) {
        return std::string();
    }
    return text.str();
}

std::string locationString(clang::SourceLocation loc, const clang::SourceManager& sm) {
    clang::PresumedLoc pl = sm.getPresumedLoc(loc);
    if (!pl.isValid()) {
        return std::string();
    }
    std::string result = pl.getFilename() ? pl.getFilename() : "";
    result += ":";
    result += std::to_string(pl.getLine());
    return result;
}

// Best-effort Itanium mangling. Falls back to the plain (demangled) name
// whenever mangling doesn't apply or anything looks off — never crashes
// the extraction over a mangling edge case.
std::string computeMangledName(clang::FunctionDecl* fd) {
    std::string plain = fd->getNameInfo().getAsString();
    if (!fd->getIdentifier()) {
        // Operators, constructors, destructors, etc. — keep it simple and
        // just use the printable name; the analysis layer only needs a
        // stable name, and `name` (the map key) is what's actually used
        // for lookups.
        return plain;
    }

    clang::ASTContext& ctx = fd->getASTContext();
    std::unique_ptr<clang::MangleContext> mc(
        clang::ItaniumMangleContext::create(ctx, ctx.getDiagnostics()));
    if (!mc || !mc->shouldMangleDeclName(fd)) {
        return plain;
    }

    std::string mangled;
    llvm::raw_string_ostream os(mangled);
    mc->mangleName(fd, os);
    os.flush();
    if (mangled.empty()) {
        return plain;
    }
    return mangled;
}

// Best-effort extraction of the "variable name" of an array being
// subscripted. Falls back to the raw source text for anything more
// complex than a simple variable/member reference (e.g. pointer
// arithmetic like `(A + 1)[i]`).
std::string arrayBaseName(clang::Expr* base,
                           const clang::SourceManager& sm,
                           const clang::LangOptions& langOpts) {
    clang::Expr* stripped = base->IgnoreParenImpCasts();
    if (auto* dre = llvm::dyn_cast<clang::DeclRefExpr>(stripped)) {
        return dre->getNameInfo().getAsString();
    }
    if (auto* me = llvm::dyn_cast<clang::MemberExpr>(stripped)) {
        return me->getMemberNameInfo().getAsString();
    }
    return getSourceText(stripped->getSourceRange(), sm, langOpts);
}

// ---------------------------------------------------------------------
// Per-function body visitor.
//
// Walks a single function body once, tracking whether it is currently
// inside a top-level `for` loop nest. It is responsible for:
//   * discovering top-level for-loop nests and filling in their
//     LoopNest metadata (location/inductionVar/tripBound/depth),
//   * classifying every ArraySubscriptExpr inside a loop nest as a
//     read or write and recording it on that LoopNest,
//   * recording every direct CallExpr in the function (REQ-F2), also
//     mirroring calls that occur inside a loop nest onto that nest's
//     callSites (needed by REQ-P2's mod/ref-vs-loop overlap check).
// ---------------------------------------------------------------------
class FunctionBodyVisitor : public clang::RecursiveASTVisitor<FunctionBodyVisitor> {
public:
    FunctionBodyVisitor(clang::ASTContext& ctx, FunctionIR& fn)
        : sm_(ctx.getSourceManager()), langOpts_(ctx.getLangOpts()), fn_(&fn) {}

    bool shouldVisitImplicitCode() const { return false; }

    // --- top-level loop discovery -------------------------------------
    bool TraverseForStmt(clang::ForStmt* fs) {
        if (!fs) {
            return true;
        }

        bool isNewTopLevel = (currentLoopIndex_ == -1);
        if (isNewTopLevel) {
            LoopNest ln;
            ln.location = locationString(fs->getForLoc(), sm_);
            fillInductionAndBound(fs, ln);
            ln.depth = 1;
            fn_->loops.push_back(std::move(ln));
            currentLoopIndex_ = static_cast<int>(fn_->loops.size()) - 1;
            nestLevel_ = 1;
        } else {
            ++nestLevel_;
            if (nestLevel_ > fn_->loops[currentLoopIndex_].depth) {
                fn_->loops[currentLoopIndex_].depth = nestLevel_;
            }
        }

        bool result =
            clang::RecursiveASTVisitor<FunctionBodyVisitor>::TraverseForStmt(fs);

        if (isNewTopLevel) {
            currentLoopIndex_ = -1;
            nestLevel_ = 0;
        } else {
            --nestLevel_;
        }
        return result;
    }

    // --- write-context tracking for array accesses ---------------------
    bool TraverseArraySubscriptExpr(clang::ArraySubscriptExpr* e) {
        if (!e) {
            return true;
        }

        if (currentLoopIndex_ != -1) {
            ArrayAccess access;
            access.arrayName = arrayBaseName(e->getBase(), sm_, langOpts_);
            access.indexExpr = getSourceText(e->getIdx()->getSourceRange(), sm_, langOpts_);
            access.isWrite = inWriteContext_;
            access.elementType = e->getType().getAsString();
            fn_->loops[currentLoopIndex_].arrayAccesses.push_back(std::move(access));
        }

        // The subscripted element's write-ness does not propagate to the
        // base pointer/array or the index expression — those are always
        // reads (e.g. in `A[B[i]] = x`, B[i] is a read even though A[...]
        // is a write).
        bool saved = inWriteContext_;
        inWriteContext_ = false;
        clang::RecursiveASTVisitor<FunctionBodyVisitor>::TraverseStmt(e->getBase());
        clang::RecursiveASTVisitor<FunctionBodyVisitor>::TraverseStmt(e->getIdx());
        inWriteContext_ = saved;
        return true;
    }

    bool TraverseBinaryOperator(clang::BinaryOperator* bo) {
        if (!bo) {
            return true;
        }
        if (bo->isAssignmentOp()) {
            bool saved = inWriteContext_;
            inWriteContext_ = true;
            clang::RecursiveASTVisitor<FunctionBodyVisitor>::TraverseStmt(bo->getLHS());
            inWriteContext_ = false;
            clang::RecursiveASTVisitor<FunctionBodyVisitor>::TraverseStmt(bo->getRHS());
            inWriteContext_ = saved;
            return true;
        }
        return clang::RecursiveASTVisitor<FunctionBodyVisitor>::TraverseBinaryOperator(bo);
    }

    bool TraverseUnaryOperator(clang::UnaryOperator* uo) {
        if (!uo) {
            return true;
        }
        if (uo->isIncrementDecrementOp()) {
            bool saved = inWriteContext_;
            inWriteContext_ = true;
            clang::RecursiveASTVisitor<FunctionBodyVisitor>::TraverseStmt(uo->getSubExpr());
            inWriteContext_ = saved;
            return true;
        }
        return clang::RecursiveASTVisitor<FunctionBodyVisitor>::TraverseUnaryOperator(uo);
    }

    // --- call sites ------------------------------------------------------
    bool VisitCallExpr(clang::CallExpr* e) {
        if (!e) {
            return true;
        }
        CallSite cs;
        if (const clang::FunctionDecl* callee = e->getDirectCallee()) {
            cs.calleeName = callee->getNameInfo().getAsString();
        } else if (clang::Expr* calleeExpr = e->getCallee()) {
            // Indirect call (function pointer, etc.) — record the callee
            // expression text; REQ-P4 treats these as UNKNOWN_CONSERVATIVE
            // further downstream.
            cs.calleeName = getSourceText(calleeExpr->getSourceRange(), sm_, langOpts_);
        }
        for (clang::Expr* arg : e->arguments()) {
            cs.argExprs.push_back(getSourceText(arg->getSourceRange(), sm_, langOpts_));
        }

        if (currentLoopIndex_ != -1) {
            fn_->loops[currentLoopIndex_].callSites.push_back(cs);
        }
        fn_->calls.push_back(std::move(cs));
        return true;
    }

private:
    void fillInductionAndBound(clang::ForStmt* fs, LoopNest& ln) {
        std::string inductionVar;

        if (clang::Stmt* init = fs->getInit()) {
            if (auto* ds = llvm::dyn_cast<clang::DeclStmt>(init)) {
                for (clang::Decl* d : ds->decls()) {
                    if (auto* vd = llvm::dyn_cast<clang::VarDecl>(d)) {
                        inductionVar = vd->getNameAsString();
                        break;
                    }
                }
            } else if (auto* bo = llvm::dyn_cast<clang::BinaryOperator>(init)) {
                if (bo->getOpcode() == clang::BO_Assign) {
                    if (auto* dre = llvm::dyn_cast<clang::DeclRefExpr>(
                            bo->getLHS()->IgnoreParenImpCasts())) {
                        inductionVar = dre->getNameInfo().getAsString();
                    }
                }
            }
        }

        if (inductionVar.empty() && fs->getCond()) {
            if (auto* bo = llvm::dyn_cast<clang::BinaryOperator>(fs->getCond())) {
                if (auto* dre = llvm::dyn_cast<clang::DeclRefExpr>(
                        bo->getLHS()->IgnoreParenImpCasts())) {
                    inductionVar = dre->getNameInfo().getAsString();
                }
            }
        }
        ln.inductionVar = inductionVar;

        std::string bound;
        if (clang::Expr* cond = fs->getCond()) {
            if (auto* bo = llvm::dyn_cast<clang::BinaryOperator>(cond)) {
                if (bo->isComparisonOp()) {
                    bound = getSourceText(bo->getRHS()->getSourceRange(), sm_, langOpts_);
                }
            }
            if (bound.empty()) {
                bound = getSourceText(cond->getSourceRange(), sm_, langOpts_);
            }
        }
        ln.tripBound = bound;
    }

    const clang::SourceManager& sm_;
    const clang::LangOptions& langOpts_;
    FunctionIR* fn_;
    int currentLoopIndex_ = -1;
    int nestLevel_ = 0;
    bool inWriteContext_ = false;
};

// ---------------------------------------------------------------------
// Top-level (translation-unit) visitor: finds every FunctionDecl and
// builds/merges its FunctionIR entry into the ProgramIR.
// ---------------------------------------------------------------------
class ProgramVisitor : public clang::RecursiveASTVisitor<ProgramVisitor> {
public:
    ProgramVisitor(clang::ASTContext& ctx, ProgramIR& ir) : ctx_(ctx), ir_(ir) {}

    // Uninstantiated template patterns have dependent types that don't
    // make sense to stringify here; implicit instantiations are skipped
    // too since they don't visit by default when this returns false.
    bool shouldVisitTemplateInstantiations() const { return false; }
    bool shouldVisitImplicitCode() const { return false; }

    bool VisitFunctionDecl(clang::FunctionDecl* fd) {
        if (!fd || fd->isImplicit() || fd->isTemplated()) {
            return true;
        }

        std::string name = fd->getNameInfo().getAsString();
        if (name.empty()) {
            return true;
        }

        FunctionIR fn;
        fn.name = name;
        fn.mangledName = computeMangledName(fd);
        fn.returnType = fd->getReturnType().getAsString();

        for (clang::ParmVarDecl* param : fd->parameters()) {
            ParamIR p;
            p.name = param->getNameAsString();
            p.type = param->getType().getAsString();
            fn.params.push_back(std::move(p));
        }

        fn.hasDefinition = fd->hasBody();

        if (fd->hasBody() && fd->isThisDeclarationADefinition()) {
            FunctionBodyVisitor bodyVisitor(ctx_, fn);
            bodyVisitor.TraverseStmt(fd->getBody());
        }

        // Multiple declarations of the same function (e.g. a prototype
        // plus its definition, or repeated extern declarations) collapse
        // into a single ProgramIR entry, keyed by name. Prefer whichever
        // one carries the actual definition (REQ-F3: decl-only entries
        // must still exist, but should be replaced once a definition is
        // seen).
        auto it = ir_.find(fn.name);
        if (it == ir_.end()) {
            ir_.emplace(fn.name, std::move(fn));
        } else if (fn.hasDefinition && !it->second.hasDefinition) {
            it->second = std::move(fn);
        }
        return true;
    }

private:
    clang::ASTContext& ctx_;
    ProgramIR& ir_;
};

class IRASTConsumer : public clang::ASTConsumer {
public:
    explicit IRASTConsumer(ProgramIR& ir) : ir_(ir) {}

    void HandleTranslationUnit(clang::ASTContext& ctx) override {
        ProgramVisitor visitor(ctx, ir_);
        visitor.TraverseDecl(ctx.getTranslationUnitDecl());
    }

private:
    ProgramIR& ir_;
};

class IRFrontendAction : public clang::ASTFrontendAction {
public:
    explicit IRFrontendAction(ProgramIR& ir) : ir_(ir) {}

    std::unique_ptr<clang::ASTConsumer> CreateASTConsumer(
        clang::CompilerInstance& /*ci*/, llvm::StringRef /*file*/) override {
        return std::make_unique<IRASTConsumer>(ir_);
    }

private:
    ProgramIR& ir_;
};

class IRFrontendActionFactory : public clang::tooling::FrontendActionFactory {
public:
    explicit IRFrontendActionFactory(ProgramIR& ir) : ir_(ir) {}

    std::unique_ptr<clang::FrontendAction> create() override {
        return std::make_unique<IRFrontendAction>(ir_);
    }

private:
    ProgramIR& ir_;
};

} // namespace

ProgramIR ClangBridge::extractIR(const std::string& filename) {
    ProgramIR ir;

    // Minimal fixed "compile command" for the single input file. We don't
    // need a real compile_commands.json (REQ-F1: accept unmodified source
    // files directly) — a FixedCompilationDatabase with just the language
    // standard flag is enough for libTooling to synthesize a CompilerInvocation.
    std::vector<std::string> args = {"-std=c++14"};

    clang::tooling::FixedCompilationDatabase compdb(".", args);
    clang::tooling::ClangTool tool(compdb, {filename});

    IRFrontendActionFactory factory(ir);

    // Intentionally ignore the return code: ClangTool::run() returns
    // non-zero on parse errors/diagnostics, but the ASTConsumer still runs
    // (Clang performs error recovery and keeps building the AST), so
    // whatever was successfully extracted is still returned rather than
    // discarded or crashing the caller.
    tool.run(&factory);

    return ir;
}

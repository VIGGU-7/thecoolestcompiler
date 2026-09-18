#pragma once
// ProfilingInstrumentor.h — Pass 1 (--instrument): REQ-PR1, REQ-PR2, REQ-PR4.
//
// Task 6 of the thecoolestcompiler project.
//
// WHAT THIS IS
// ------------
// Given a ProgramIR (Task 2 output) and the set of loop locations already
// classified PARALLEL_SAFE (Task 5, passed in decoupled from this module —
// see below), this module produces a transformed copy of the *original*
// C++ source text with profile_loop_start()/profile_loop_end() calls
// inserted immediately before/after each PARALLEL_SAFE loop, plus a
// leading `#include "profiler.h"`.
//
// WHY TEXT-LEVEL, NOT AST-LEVEL
// ------------------------------
// This ROSE build has no C/C++ frontend of its own; the "real" AST is a
// SgProject reconstructed from the same ProgramIR by RoseBridge (Task 3)
// via SageBuilder. This module does not have access to that reconstructed
// AST (nor its unparse), so it cannot use SageInterface::insertStatement*
// to do the instrumentation at the AST level. Instead, it re-reads the
// original source file itself (hence the `originalSourcePath` parameter —
// ClangBridge's ProgramIR does not carry raw source text) and performs a
// best-effort textual insertion keyed off LoopNest::location
// ("filename:line").
//
// DECOUPLING FROM ParallelismChecker (Task 5)
// --------------------------------------------
// Rather than depending on the ParallelismChecker module/header directly,
// this module accepts a plain `std::set<std::string>` of loop locations
// already known to be PARALLEL_SAFE. The caller (translator.cpp / main())
// is responsible for running the real classifier and building that set.
//
// LIMITATIONS (best-effort text transformer — documented per spec)
// ------------------------------------------------------------------
//  1. Loop end detection: starting just after the loop header's matching
//     ')', this scans forward for the first non-whitespace character.
//       - If it is '{', brace-depth counting locates the matching '}';
//         that line is treated as the loop's end line.
//       - Otherwise (a brace-less single-statement body), the next ';'
//         is treated as the end of the loop body; the line containing it
//         is treated as the loop's end line.
//     This is a simple scan, not a real parser: pathological formatting
//     (braces/semicolons inside string/char literals or comments, loop
//     headers containing lambdas with their own braces, etc.) can confuse
//     it. Per the task spec this is acceptable ("OK if this doesn't
//     handle deeply pathological formatting").
//  2. If a loop's header can't be located on its reported line (e.g. the
//     line doesn't contain "for" or has no '(' at all), that loop is
//     silently skipped (left uninstrumented) rather than producing
//     malformed output.
//  3. Estimates (trip_count/flops/bytes_read/bytes_written) are static,
//     compile-time approximations per REQ-PR2 — see estimateLoop() below
//     for the exact formulas. They are not derived from the loop's real
//     arithmetic expressions (ClangBridge's IR does not carry full
//     expression ASTs, only array-access lists), so they are intentionally
//     coarse.
//  4. Multiple loops that resolve to the same start/end line (unusual,
//     e.g. one-line "for (...) for (...) body;" nests) each get their own
//     insertion; ordering among them follows ProgramIR's (function name,
//     then loop vector) iteration order.

#include "ClangBridge.h"

#include <cstddef>
#include <set>
#include <string>
#include <vector>

class ProfilingInstrumentor {
public:
    // Reads originalSourcePath from disk, inserts profile_loop_start(...)/
    // profile_loop_end(...) calls around every loop in `ir` whose
    // LoopNest::location is a member of parallelSafeLoopLocations, and
    // returns the transformed source (with a leading
    // `#include "profiler.h"`) as a string. Does not write any files
    // itself — the caller is expected to pass the result to
    // RoseBridge/backend() or write it out directly per REQ-PR4.
    //
    // If originalSourcePath cannot be opened, returns a minimal
    // compilable stub (just the #include) rather than throwing.
    static std::string instrument(const std::string& originalSourcePath,
                                   const ProgramIR& ir,
                                   const std::set<std::string>& parallelSafeLoopLocations);

private:
    // Static compile-time estimate for one loop's profiling counters.
    // See ProfilingInstrumentor.cpp for the exact formulas (REQ-PR2).
    struct LoopEstimate {
        long tripCount;
        long flops;
        long bytesRead;
        long bytesWritten;
    };

    static LoopEstimate estimateLoop(const LoopNest& loop);

    // Parses s as a base-10 (optionally negative) integer literal.
    // Returns false if s is not purely numeric (e.g. an identifier like
    // "N" or an expression like "n+1").
    static bool parseNumericLiteral(const std::string& s, long& out);

    // Escapes backslashes/quotes so `s` can be embedded inside a C++
    // string literal in the generated source.
    static std::string escapeForCxxLiteral(const std::string& s);

    // Given sorted line-start byte offsets into a text buffer, returns the
    // 0-based index of the line containing byte offset `offset`.
    static std::size_t lineIndexOfOffset(const std::vector<std::size_t>& lineStarts,
                                          std::size_t offset);
};

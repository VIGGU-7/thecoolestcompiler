#pragma once
// RoseBridge.h — Converts the ClangBridge ProgramIR into a ROSE SgProject
//
// IMPORTANT: This ROSE build has NO EDG C++ frontend.
//   - Do NOT call frontend() or ROSE_INITIALIZE with a source file.
//   - The SgProject / SgSourceFile are built programmatically via SageBuilder.
//
// Downstream consumers (Tasks 4–8) receive an SgProject* that passes
// AstTests::runAllTests() and contains:
//   - SgFunctionDeclaration nodes (defining + non-defining) in global scope
//   - SgForStatement nodes representing each LoopNest
//   - symbol-table entries wired up by SageBuilder

#include "ClangBridge.h"
#include "rose.h"

#include <map>
#include <string>
#include <iostream>

class RoseBridge {
public:
    // -----------------------------------------------------------------------
    // Build a SgProject from the extracted ProgramIR.
    // The project is heap-allocated; the caller is responsible for deleting it
    // (or passing ownership to backend()/unparse()).
    // -----------------------------------------------------------------------
    static SgProject* buildSageAST(const ProgramIR& ir);

    // -----------------------------------------------------------------------
    // Map from loop location string ("file:line") to the SgForStatement
    // built for it.  Populated by buildSageAST().
    // -----------------------------------------------------------------------
    static std::map<std::string, SgForStatement*> loopMap;

    // -----------------------------------------------------------------------
    // Map from function name to SgFunctionDeclaration (defining decl).
    // Non-defining (forward) decls are not stored here.
    // Populated by buildSageAST().
    // -----------------------------------------------------------------------
    static std::map<std::string, SgFunctionDeclaration*> funcMap;

    // -----------------------------------------------------------------------
    // Print a human-readable summary of the built AST to the given stream.
    // Used by the --dump-sage-ast CLI flag in translator.cpp.
    // -----------------------------------------------------------------------
    static void dumpAST(std::ostream& out = std::cout);
};

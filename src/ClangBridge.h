#pragma once
// ClangBridge.h — Clang-extracted IR data structures
// NO ROSE dependency. Pure C++ structs only.
//
// These are populated by ClangBridge::extractIR() (Task 2) and consumed by
// RoseBridge::buildSageAST() (Task 3) and the interprocedural analysis (Task 4).

#include <string>
#include <vector>
#include <map>

// ---------------------------------------------------------------------------
// Array access within a loop body
// ---------------------------------------------------------------------------
struct ArrayAccess {
    std::string arrayName;       // variable name of the array
    std::string indexExpr;       // textual index expression (e.g. "i", "i+1")
    bool        isWrite;         // true = written, false = read-only
    std::string elementType;     // "double", "float", "int", etc.
};

// ---------------------------------------------------------------------------
// A function call site observed inside a loop (or function body)
// ---------------------------------------------------------------------------
struct CallSite {
    std::string calleeName;              // demangled callee name
    std::vector<std::string> argExprs;   // textual argument expressions
};

// ---------------------------------------------------------------------------
// A single loop nest (outermost loop level)
// ---------------------------------------------------------------------------
struct LoopNest {
    std::string location;        // "filename:line" from Clang source info
    std::string inductionVar;    // loop induction variable name (e.g. "i")
    std::string tripBound;       // trip-count expression (e.g. "N", "1024")
    int         depth;           // nesting depth (1 = single loop)

    std::vector<ArrayAccess> arrayAccesses;  // all array accesses inside loop
    std::vector<CallSite>    callSites;      // all direct calls inside loop
};

// ---------------------------------------------------------------------------
// A function parameter
// ---------------------------------------------------------------------------
struct ParamIR {
    std::string name;   // parameter name
    std::string type;   // type string (e.g. "double *", "int", "float *")
};

// ---------------------------------------------------------------------------
// Per-function IR extracted by the Clang bridge
// ---------------------------------------------------------------------------
struct FunctionIR {
    std::string name;              // demangled function name
    std::string mangledName;       // mangled name (may equal name for C)
    std::string returnType;        // return type string

    std::vector<ParamIR>   params;     // parameter list in order
    std::vector<LoopNest>  loops;      // all top-level loop nests
    std::vector<CallSite>  calls;      // all direct calls in function body
    bool                   hasDefinition; // false = opaque (library/extern)
};

// ---------------------------------------------------------------------------
// Whole-program IR: function name → FunctionIR
// ---------------------------------------------------------------------------
using ProgramIR = std::map<std::string, FunctionIR>;

// ---------------------------------------------------------------------------
// ClangBridge — Task 2 entry point. Extracts a ProgramIR from a C/C++
// source file using Clang's libtooling / RecursiveASTVisitor (NOT the C
// API). Implemented in ClangBridge.cpp; has zero ROSE dependency.
// ---------------------------------------------------------------------------
class ClangBridge {
public:
    // Parses `filename` with Clang and returns the extracted ProgramIR.
    // Never throws and never crashes on malformed/partial input: if the
    // file does not parse cleanly, whatever could be extracted around the
    // error is still returned.
    static ProgramIR extractIR(const std::string& filename);
};

// translator.cpp — main() pipeline driver / CLI entry point.
//
// Wires together: ClangBridge (Task 2) -> RoseBridge (Task 3) ->
// InterproceduralAnalysis (Task 4) -> ParallelismChecker (Task 5) ->
// ProfilingInstrumentor (Task 6) / GpuSuitabilityEngine (Task 7) ->
// OmpCodegen (Task 8), per the CLI contract in PRD.md section 7.

#include "ClangBridge.h"
#include "RoseBridge.h"
#include "InterproceduralAnalysis.h"
#include "ParallelismChecker.h"
#include "ProfilingInstrumentor.h"
#include "GpuSuitabilityEngine.h"
#include "OmpCodegen.h"

#include "rose.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <set>
#include <sstream>
#include <string>

namespace {

struct CliOptions {
    std::string inputPath;
    std::string outputPath;
    bool instrument = false;
    bool haveProfile = false;
    std::string profilePath;
    GpuHardwareParams hw;
    bool dumpIr = false;
    bool dumpSageAst = false;
    bool dumpModref = false;
    bool checkParallel = false;
    bool dumpGpuDecision = false;
    bool astCodegen = false;
    bool help = false;
};

void printUsage(std::ostream& out) {
    out <<
        "Usage: translator [OPTIONS] <input.cpp>\n"
        "\n"
        "thecoolestcompiler — interprocedural GPU-offload parallelizing compiler.\n"
        "\n"
        "Options:\n"
        "  --instrument              Pass 1: insert profiling instrumentation\n"
        "  --profile=<file>          Pass 2: read profile JSON for GPU suitability\n"
        "  --gpu-peak-flops=<N>      GPU peak FLOP/s in GFLOPS (default: 10000)\n"
        "  --gpu-peak-bw=<N>         GPU peak memory bandwidth in GB/s (default: 900)\n"
        "  --min-gpu-trips=<N>       Minimum trip count for GPU offload (default: 10000)\n"
        "  -o <output.cpp>           Output file (default: rose_<input>.cpp)\n"
        "  --dump-ir                 Print extracted Clang IR to stdout and exit\n"
        "  --dump-sage-ast           Print reconstructed ROSE AST summary and exit\n"
        "  --dump-modref             Print interprocedural mod/ref tables and exit\n"
        "  --check-parallel          Print loop parallelism classification and exit\n"
        "  --dump-gpu-decision       Print GPU suitability decisions and exit\n"
        "  --ast-codegen             Use the ROSE-AST unparse codegen path instead of\n"
        "                            the default text-level path (currently produces\n"
        "                            non-compilable output — see CONTEXT.md)\n"
        "  --help                    Show this message\n";
}

bool startsWith(const std::string& s, const std::string& prefix) {
    return s.compare(0, prefix.size(), prefix) == 0;
}

CliOptions parseArgs(int argc, char** argv) {
    CliOptions opt;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--help" || arg == "-h") {
            opt.help = true;
        } else if (arg == "--instrument") {
            opt.instrument = true;
        } else if (startsWith(arg, "--profile=")) {
            opt.haveProfile = true;
            opt.profilePath = arg.substr(std::strlen("--profile="));
        } else if (startsWith(arg, "--gpu-peak-flops=")) {
            opt.hw.peakFlopsGFLOPs = std::atof(arg.c_str() + std::strlen("--gpu-peak-flops="));
        } else if (startsWith(arg, "--gpu-peak-bw=")) {
            opt.hw.peakBandwidthGBs = std::atof(arg.c_str() + std::strlen("--gpu-peak-bw="));
        } else if (startsWith(arg, "--min-gpu-trips=")) {
            opt.hw.minGpuTrips = std::atol(arg.c_str() + std::strlen("--min-gpu-trips="));
        } else if (arg == "-o") {
            if (i + 1 < argc) {
                opt.outputPath = argv[++i];
            }
        } else if (arg == "--dump-ir") {
            opt.dumpIr = true;
        } else if (arg == "--dump-sage-ast") {
            opt.dumpSageAst = true;
        } else if (arg == "--dump-modref") {
            opt.dumpModref = true;
        } else if (arg == "--check-parallel") {
            opt.checkParallel = true;
        } else if (arg == "--dump-gpu-decision") {
            opt.dumpGpuDecision = true;
        } else if (arg == "--ast-codegen") {
            opt.astCodegen = true;
        } else if (!arg.empty() && arg[0] != '-') {
            opt.inputPath = arg;
        }
        // Unrecognized flags are ignored rather than treated as fatal, so the
        // tool degrades gracefully as new options are added.
    }
    return opt;
}

std::string defaultOutputPath(const std::string& inputPath) {
    std::string base = inputPath;
    size_t slash = base.find_last_of('/');
    if (slash != std::string::npos) {
        base = base.substr(slash + 1);
    }
    return "rose_" + base;
}

void dumpIr(const ProgramIR& ir, std::ostream& out) {
    for (const auto& kv : ir) {
        const FunctionIR& f = kv.second;
        out << "Function: " << f.name << " (" << f.mangledName << ") -> " << f.returnType
            << " hasDefinition=" << (f.hasDefinition ? "true" : "false") << "\n";
        for (const auto& p : f.params) {
            out << "  param: " << p.type << " " << p.name << "\n";
        }
        for (const auto& loop : f.loops) {
            out << "  loop @ " << loop.location << " induction=" << loop.inductionVar
                << " bound=" << loop.tripBound << " depth=" << loop.depth << "\n";
            for (const auto& acc : loop.arrayAccesses) {
                out << "    access " << acc.arrayName << "[" << acc.indexExpr << "] "
                    << (acc.isWrite ? "WRITE" : "READ") << " type=" << acc.elementType << "\n";
            }
            for (const auto& cs : loop.callSites) {
                out << "    call " << cs.calleeName << "(...)\n";
            }
        }
        for (const auto& cs : f.calls) {
            out << "  calls " << cs.calleeName << "(...)\n";
        }
    }
}

// Builds a location -> LoopVerdict map from ParallelismChecker::classifyAll's
// per-function, by-loop-index result, keyed the same way every other module
// keys per-loop data ("file:line", i.e. LoopNest::location).
std::map<std::string, LoopVerdict> verdictsByLocation(
    const ProgramIR& ir,
    const std::map<std::string, std::vector<LoopVerdict>>& byFunction) {
    std::map<std::string, LoopVerdict> byLocation;
    for (const auto& kv : ir) {
        const FunctionIR& f = kv.second;
        auto it = byFunction.find(f.name);
        if (it == byFunction.end()) continue;
        const std::vector<LoopVerdict>& verdicts = it->second;
        for (size_t i = 0; i < f.loops.size() && i < verdicts.size(); ++i) {
            byLocation[f.loops[i].location] = verdicts[i];
        }
    }
    return byLocation;
}

void dumpGpuDecisions(const std::map<std::string, SuitabilityResult>& results, std::ostream& out) {
    for (const auto& kv : results) {
        const SuitabilityResult& r = kv.second;
        out << kv.first << ": "
            << (r.decision == GpuDecision::GPU_PROFITABLE ? "GPU_PROFITABLE" : "CPU_PREFERRED")
            << " (intensity=" << r.flopIntensity << " ridge=" << r.ridgePoint
            << " trips=" << r.tripCount << ") " << r.reason << "\n";
    }
}

// Parses the line number out of a LoopNest::location string ("file:line").
// Returns -1 if it can't be parsed.
int parseLocationLine(const std::string& location) {
    size_t colon = location.find_last_of(':');
    if (colon == std::string::npos) return -1;
    try {
        return std::stoi(location.substr(colon + 1));
    } catch (...) {
        return -1;
    }
}

// Text-level OpenMP codegen: inserts "#pragma omp ..." directly above each
// decided loop's source line, on the ORIGINAL source text.
//
// This exists because the AST-based path (OmpCodegen::applyPragmas() +
// ROSE's backend() unparser, operating on RoseBridge's SageBuilder-
// reconstructed AST) produces syntactically-valid but semantically-empty
// output: RoseBridge rebuilds loop bodies as placeholder variable
// declarations only (see RoseBridge.cpp's file-header comment), not the
// real statements, so the unparsed result drops the actual program logic
// (e.g. `const int N = ...;`) and doesn't compile, let alone run correctly.
// See CONTEXT.md "Known remaining gaps" #1 for the full writeup.
//
// The text-level approach mirrors ProfilingInstrumentor's already-verified
// technique (Task 6): read the real source, locate each affected loop by its
// "file:line" location, and insert one line immediately before it. Unlike
// instrumentation this only needs the loop's START line (a pragma always
// goes immediately above the `for`), so there's no brace-matching needed.
// The result is genuinely compilable, numerically-correct-preserving output
// — necessary for a real, measurable speedup demo.
std::string emitTextLevelOmp(const std::string& originalSourcePath,
                              const ProgramIR& ir,
                              const std::map<std::string, OmpDecision>& decisions) {
    std::ifstream in(originalSourcePath);
    std::vector<std::string> lines;
    std::string line;
    while (std::getline(in, line)) {
        lines.push_back(line);
    }

    // location -> (LoopNest*, pragma text), so we can build the pragma
    // string using the real loop data (array accesses, trip bound) via
    // OmpCodegen::buildPragmaText — the same logic the AST path uses,
    // just applied as a text insertion instead of an AST insertion.
    std::map<int, std::string> pragmaByLine; // 1-based source line -> pragma text
    for (const auto& kv : ir) {
        const FunctionIR& f = kv.second;
        for (const LoopNest& loop : f.loops) {
            auto dIt = decisions.find(loop.location);
            if (dIt == decisions.end()) continue;
            int lineNo = parseLocationLine(loop.location);
            if (lineNo < 1) continue;
            std::string pragmaBody = OmpCodegen::buildPragmaText(loop, dIt->second);
            pragmaByLine[lineNo] = "#pragma " + pragmaBody;
        }
    }

    std::ostringstream out;
    for (size_t i = 0; i < lines.size(); ++i) {
        int lineNo = static_cast<int>(i) + 1;
        auto it = pragmaByLine.find(lineNo);
        if (it != pragmaByLine.end()) {
            // Preserve the original line's leading indentation for the
            // pragma so the output reads naturally.
            size_t indentEnd = lines[i].find_first_not_of(" \t");
            std::string indent = (indentEnd == std::string::npos)
                                      ? ""
                                      : lines[i].substr(0, indentEnd);
            out << indent << it->second << "\n";
        }
        out << lines[i] << "\n";
    }
    return out.str();
}

} // namespace

int main(int argc, char** argv) {
    CliOptions opt = parseArgs(argc, argv);

    if (opt.help || opt.inputPath.empty()) {
        printUsage(std::cout);
        return opt.help ? 0 : 1;
    }
    if (opt.outputPath.empty()) {
        opt.outputPath = defaultOutputPath(opt.inputPath);
    }

    // ---- Task 2: Clang frontend -> ProgramIR ------------------------------
    ProgramIR ir = ClangBridge::extractIR(opt.inputPath);

    if (opt.dumpIr) {
        dumpIr(ir, std::cout);
        return 0;
    }

    // ---- Task 3: ProgramIR -> reconstructed ROSE SgProject -----------------
    SgProject* project = RoseBridge::buildSageAST(ir);

    if (opt.dumpSageAst) {
        RoseBridge::dumpAST(std::cout);
        return 0;
    }

    // ---- Task 4: interprocedural call graph + mod/ref propagation --------
    InterproceduralAnalysis interproc;
    interproc.analyze(project, ir);

    if (opt.dumpModref) {
        interproc.dump(std::cout);
        return 0;
    }

    // ---- Task 5: parallelism classification --------------------------------
    ParallelismChecker checker;

    if (opt.checkParallel) {
        checker.printReport(ir, interproc, std::cout);
        return 0;
    }

    std::map<std::string, std::vector<LoopVerdict>> verdictsByFunc =
        checker.classifyAll(ir, interproc);
    std::map<std::string, LoopVerdict> verdicts = verdictsByLocation(ir, verdictsByFunc);

    // ---- Task 6: Pass 1 — profiling instrumentation ------------------------
    if (opt.instrument) {
        std::set<std::string> parallelSafeLoopLocations;
        for (const auto& kv : verdicts) {
            if (kv.second.classification == PARALLEL_SAFE) {
                parallelSafeLoopLocations.insert(kv.first);
            }
        }
        std::string instrumented =
            ProfilingInstrumentor::instrument(opt.inputPath, ir, parallelSafeLoopLocations);
        std::ofstream out(opt.outputPath);
        if (!out) {
            std::cerr << "error: could not write output file " << opt.outputPath << "\n";
            return 1;
        }
        out << instrumented;
        std::cout << "Instrumented output written to " << opt.outputPath << "\n";
        return 0;
    }

    // ---- Task 7: Pass 2 — GPU suitability decision (roofline model) -------
    std::map<std::string, SuitabilityResult> gpuDecisions;
    if (opt.haveProfile) {
        gpuDecisions = GpuSuitabilityEngine::classifyFromProfile(opt.profilePath, opt.hw);
        if (gpuDecisions.empty()) {
            std::cerr << "warning: could not read profile " << opt.profilePath
                      << " — falling back to static estimates\n";
            gpuDecisions = GpuSuitabilityEngine::classifyStatic(ir, opt.hw);
        }
    } else {
        gpuDecisions = GpuSuitabilityEngine::classifyStatic(ir, opt.hw);
    }

    if (opt.dumpGpuDecision) {
        dumpGpuDecisions(gpuDecisions, std::cout);
        return 0;
    }

    // ---- Task 8: OpenMP codegen ---------------------------------------------
    // REQ-C5: only PARALLEL_SAFE / REDUCTION_CANDIDATE loops are ever placed
    // in `decisions` — SERIAL_ONLY / UNKNOWN_CONSERVATIVE loops are left
    // completely untransformed because OmpCodegen only ever touches loops
    // present in this map.
    std::map<std::string, OmpDecision> decisions;
    for (const auto& kv : verdicts) {
        const std::string& location = kv.first;
        const LoopVerdict& v = kv.second;
        if (v.classification != PARALLEL_SAFE && v.classification != REDUCTION_CANDIDATE) {
            continue;
        }
        OmpDecision d;
        auto gpuIt = gpuDecisions.find(location);
        d.gpuProfitable = (gpuIt != gpuDecisions.end() &&
                            gpuIt->second.decision == GpuDecision::GPU_PROFITABLE);
        d.hasReduction = (v.classification == REDUCTION_CANDIDATE);
        d.reductionVar = v.reductionVar;
        d.reductionOp = v.reductionOp.empty() ? "+" : v.reductionOp;
        decisions[location] = d;
    }

    if (opt.astCodegen) {
        // Opt-in path (--ast-codegen): apply pragmas to the SageBuilder-
        // reconstructed AST and unparse via ROSE's backend(). Known broken
        // for compilability today (RoseBridge's loop bodies are placeholder
        // declarations, not real statements) — kept available for anyone
        // continuing work on RoseBridge's body-reconstruction fidelity.
        OmpCodegen::applyPragmas(project, ir, decisions);
        int backendStatus = backend(project);
        if (backendStatus != 0) {
            std::cerr << "warning: ROSE backend() returned nonzero status " << backendStatus << "\n";
        }
        if (std::rename("rose_reconstructed.cpp", opt.outputPath.c_str()) != 0) {
            std::ifstream src("rose_reconstructed.cpp");
            if (src) {
                std::ofstream dst(opt.outputPath);
                dst << src.rdbuf();
            } else {
                std::cerr << "error: ROSE backend did not produce rose_reconstructed.cpp\n";
                return 1;
            }
        }
        std::cout << "Transformed output (AST codegen) written to " << opt.outputPath << "\n";
        return 0;
    }

    // ---- Default: text-level codegen on the real original source ---------
    // Produces genuinely compilable, logic-preserving output. See
    // emitTextLevelOmp()'s comment for why this is the default instead of
    // the AST-unparse path.
    std::string transformed = emitTextLevelOmp(opt.inputPath, ir, decisions);
    std::ofstream out(opt.outputPath);
    if (!out) {
        std::cerr << "error: could not write output file " << opt.outputPath << "\n";
        return 1;
    }
    out << transformed;
    std::cout << "Transformed output written to " << opt.outputPath << "\n";
    return 0;
}

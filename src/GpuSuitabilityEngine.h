#pragma once
// GpuSuitabilityEngine.h — Pass 2, the GPU Suitability Decision Engine
// (REQ-G1..REQ-G4).
//
// Task 7 of the thecoolestcompiler project.
//
// Two entry points:
//   - classifyFromProfile(): REQ-G1/G2. Reads the profile.json produced by
//     Pass 1 (ProfilingInstrumentor + support/profiler.c at runtime) and
//     applies the roofline model to each recorded loop.
//   - classifyStatic(): REQ-G4. Used when no --profile=<path> was given on
//     the CLI; estimates flops/bytes/trip-count directly from the IR
//     (same coarse formulas as ProfilingInstrumentor's static estimates —
//     see GpuSuitabilityEngine.cpp) instead of from a runtime profile.
//
// This module only depends on ClangBridge.h (for ProgramIR) and
// nlohmann/json.hpp (bundled with the ROSE install, for reading
// profile.json). It deliberately does NOT include rose.h or
// InterproceduralAnalysis.h, so it can be built and unit-tested without a
// ROSE-dependent toolchain.

#include "ClangBridge.h"

#include <map>
#include <string>

// ---------------------------------------------------------------------------
// Decision + result types
// ---------------------------------------------------------------------------
enum class GpuDecision { GPU_PROFITABLE, CPU_PREFERRED };

struct SuitabilityResult {
    GpuDecision decision;
    double      flopIntensity; // FLOP/byte = flops / (bytes_read + bytes_written)
    double      ridgePoint;    // FLOP/byte = peakFlopsGFLOPs / peakBandwidthGBs
    long        tripCount;
    std::string reason;        // human-readable justification (for --dump-gpu-decision)
};

// REQ-G3: default hardware constants, overridable via CLI
// (--gpu-peak-flops=, --gpu-peak-bw=, --min-gpu-trips=).
struct GpuHardwareParams {
    double peakFlopsGFLOPs = 10000; // GFLOP/s
    double peakBandwidthGBs = 900;  // GB/s
    long   minGpuTrips = 10000;
};

// ---------------------------------------------------------------------------
// GpuSuitabilityEngine
// ---------------------------------------------------------------------------
class GpuSuitabilityEngine {
public:
    // REQ-G1/G2: profile-guided classification. Reads the JSON array at
    // profileJsonPath (schema produced by support/profiler.c — see
    // profiler.h), keyed by "loop_id" (== LoopNest::location), and applies
    // the roofline model (REQ-G2) to each entry using `hw`.
    //
    // Returns an empty map if the file can't be opened or parsed (caller
    // may want to fall back to classifyStatic() in that case, or report an
    // error — this function does not throw).
    static std::map<std::string, SuitabilityResult> classifyFromProfile(
        const std::string& profileJsonPath,
        const GpuHardwareParams& hw);

    // REQ-G4: static fallback used when no --profile is given. Estimates
    // trip count / flops / bytes directly from `ir`:
    //   - numeric tripBound  -> usable trip count + a rough static
    //     arithmetic-intensity estimate, then the normal roofline test.
    //   - non-constant tripBound -> always CPU_PREFERRED (REQ-G4, since a
    //     trip count can't be known safe for GPU dispatch overhead without
    //     runtime data).
    static std::map<std::string, SuitabilityResult> classifyStatic(
        const ProgramIR& ir,
        const GpuHardwareParams& hw);

private:
    // Shared roofline decision (REQ-G2) given already-computed totals.
    static SuitabilityResult classifyOne(double flops,
                                          double bytesRead,
                                          double bytesWritten,
                                          long tripCount,
                                          const GpuHardwareParams& hw);

    // Parses s as a base-10 (optionally signed) integer literal; false if
    // s is not purely numeric (e.g. an identifier like "N").
    static bool parseNumericLiteral(const std::string& s, long& out);
};

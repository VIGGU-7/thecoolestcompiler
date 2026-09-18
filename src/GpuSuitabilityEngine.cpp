// GpuSuitabilityEngine.cpp — Pass 2 implementation (REQ-G1..REQ-G4).

#include "GpuSuitabilityEngine.h"

#include <cctype>
#include <fstream>
#include <sstream>

#include <nlohmann/json.hpp>

bool GpuSuitabilityEngine::parseNumericLiteral(const std::string& s, long& out) {
    std::size_t begin = 0;
    std::size_t end = s.size();
    while (begin < end && std::isspace(static_cast<unsigned char>(s[begin]))) ++begin;
    while (end > begin && std::isspace(static_cast<unsigned char>(s[end - 1]))) --end;
    if (begin >= end) return false;

    std::size_t idx = begin;
    if (s[idx] == '-' || s[idx] == '+') ++idx;
    if (idx >= end) return false;
    for (std::size_t k = idx; k < end; ++k) {
        if (!std::isdigit(static_cast<unsigned char>(s[k]))) return false;
    }

    try {
        out = std::stol(s.substr(begin, end - begin));
    } catch (...) {
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Roofline model (REQ-G2), exactly as specified:
//   flop_intensity = flops / (bytes_read + bytes_written)     [FLOP/byte]
//   ridge_point    = peakFlopsGFLOPs / peakBandwidthGBs       [FLOP/byte]
//   GPU_PROFITABLE iff flop_intensity >= ridge_point AND trip_count >= minGpuTrips
// ---------------------------------------------------------------------------
SuitabilityResult GpuSuitabilityEngine::classifyOne(double flops,
                                                     double bytesRead,
                                                     double bytesWritten,
                                                     long tripCount,
                                                     const GpuHardwareParams& hw) {
    SuitabilityResult r;
    double bytesTotal = bytesRead + bytesWritten;
    r.flopIntensity = (bytesTotal > 0.0) ? (flops / bytesTotal) : 0.0;
    r.ridgePoint = (hw.peakBandwidthGBs > 0.0) ? (hw.peakFlopsGFLOPs / hw.peakBandwidthGBs) : 0.0;
    r.tripCount = tripCount;

    bool profitable = (r.flopIntensity >= r.ridgePoint) && (tripCount >= hw.minGpuTrips);
    r.decision = profitable ? GpuDecision::GPU_PROFITABLE : GpuDecision::CPU_PREFERRED;

    std::ostringstream reason;
    reason << "flop_intensity=" << r.flopIntensity
           << " ridge_point=" << r.ridgePoint
           << " trip_count=" << tripCount
           << " min_gpu_trips=" << hw.minGpuTrips
           << " -> " << (profitable ? "GPU_PROFITABLE" : "CPU_PREFERRED");
    r.reason = reason.str();

    return r;
}

// ---------------------------------------------------------------------------
// classifyFromProfile() — REQ-G1/G2
// ---------------------------------------------------------------------------
std::map<std::string, SuitabilityResult> GpuSuitabilityEngine::classifyFromProfile(
    const std::string& profileJsonPath,
    const GpuHardwareParams& hw) {
    std::map<std::string, SuitabilityResult> results;

    std::ifstream in(profileJsonPath);
    if (!in) {
        return results; // caller may fall back to classifyStatic()
    }

    nlohmann::json j;
    try {
        in >> j;
    } catch (...) {
        return results;
    }

    if (!j.is_array()) {
        return results;
    }

    for (const auto& entry : j) {
        if (!entry.is_object()) continue;
        std::string loopId = entry.value("loop_id", std::string());
        if (loopId.empty()) continue;

        double flops = entry.value("flops", 0.0);
        double bytesRead = entry.value("bytes_read", 0.0);
        double bytesWritten = entry.value("bytes_written", 0.0);
        long tripCount = entry.value("trip_count", static_cast<long>(0));

        results[loopId] = classifyOne(flops, bytesRead, bytesWritten, tripCount, hw);
    }

    return results;
}

// ---------------------------------------------------------------------------
// classifyStatic() — REQ-G4
// ---------------------------------------------------------------------------
// Uses the same coarse static-estimate formulas as
// ProfilingInstrumentor::estimateLoop() (accessCount * 2 for flops, 8
// bytes/access for bytes_read/bytes_written, both scaled by the numeric
// trip bound) so that the static fallback path and the profile-guided path
// are directionally consistent with each other.
std::map<std::string, SuitabilityResult> GpuSuitabilityEngine::classifyStatic(
    const ProgramIR& ir,
    const GpuHardwareParams& hw) {
    std::map<std::string, SuitabilityResult> results;

    for (const auto& fnEntry : ir) {
        for (const LoopNest& loop : fnEntry.second.loops) {
            long bound = 0;
            bool numeric = parseNumericLiteral(loop.tripBound, bound);

            if (!numeric) {
                // REQ-G4: non-constant trip bound -> always CPU_PREFERRED.
                SuitabilityResult r;
                r.tripCount = 0;
                r.flopIntensity = 0.0;
                r.ridgePoint = (hw.peakBandwidthGBs > 0.0)
                                   ? (hw.peakFlopsGFLOPs / hw.peakBandwidthGBs)
                                   : 0.0;
                r.decision = GpuDecision::CPU_PREFERRED;
                r.reason = "non-constant trip bound '" + loop.tripBound +
                           "' -> static fallback forces CPU_PREFERRED (REQ-G4)";
                results[loop.location] = r;
                continue;
            }

            long accessCount = static_cast<long>(loop.arrayAccesses.size());
            long reads = 0;
            long writes = 0;
            for (const ArrayAccess& a : loop.arrayAccesses) {
                if (a.isWrite) ++writes;
                else ++reads;
            }

            double flops = static_cast<double>(accessCount * 2) * static_cast<double>(bound);
            double bytesRead = static_cast<double>(reads * 8) * static_cast<double>(bound);
            double bytesWritten = static_cast<double>(writes * 8) * static_cast<double>(bound);

            results[loop.location] = classifyOne(flops, bytesRead, bytesWritten, bound, hw);
        }
    }

    return results;
}

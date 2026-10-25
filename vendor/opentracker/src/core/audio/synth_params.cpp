// Synth parameters by index and the macro law (synth_params.h).
#include "audio/synth_params.h"

#include <cmath>

#include "formats/patch_pb.h"

namespace ot {
namespace audio {

int synthParamCount(int32_t engine) {
    int n = 0;
    patchEngineParams(engine, &n);
    return n;
}

const PatchParamRange* synthParamRange(int32_t engine, int index) {
    int n = 0;
    const PatchParamRange* t = patchEngineParams(engine, &n);
    return (t && index >= 0 && index < n) ? &t[index] : nullptr;
}

float synthParamClamp(int32_t engine, int index, float v) {
    const PatchParamRange* r = synthParamRange(engine, index);
    if (!r || (r->flags & kParamFlagNoRange)) return v;
    return v < r->min ? r->min : (v > r->max ? r->max : v);
}

float synthParamGet(const SynthPatch& p, int index) {
    const PatchParamRange* r = synthParamRange(p.engine, index);
    if (!r) return 0.0f;
    if (index < p.paramCount && p.params[index].present) {
        const float v = patchBitsToFloat(p.params[index].bits);
        if (patchParamInRange(p.engine, index, v)) return v;
    }
    return r->def;
}

bool synthParamSet(SynthPatch& p, int index, float v) {
    const PatchParamRange* r = synthParamRange(p.engine, index);
    if (!r || !std::isfinite(v)) return false;
    v = synthParamClamp(p.engine, index, v);
    if (index >= p.paramCount) {
        // Grow to the engine count; new entries keep the default (absent).
        const int n = synthParamCount(p.engine);
        for (int i = p.paramCount; i < n; ++i) p.params[i] = PatchParam{0u, false};
        p.paramCount = uint16_t(n);
    }
    p.params[index].bits = patchFloatToBits(v);
    p.params[index].present = true;
    return true;
}

int synthMacroTargets(const PatchMacro& m) {
    return m.count < 0 ? 0 : (m.count > kPatchMacroTargets ? kPatchMacroTargets : int(m.count));
}

int synthEffectiveParams(const SynthPatch& p, const uint8_t values[kSynthSeqMacros], float velocity, float* out) {
    int n = 0;
    const PatchParamRange* t = patchEngineParams(p.engine, &n);
    if (!t) return 0;
    for (int i = 0; i < n; ++i) out[i] = synthParamGet(p, i);
    // M1..M6: additive, scaled by the parameter range (U8).
    float add[kPatchMaxParams] = {};
    bool hit[kPatchMaxParams] = {};
    for (int k = 0; k < kSynthSeqMacros; ++k) {
        const PatchMacro& m = p.macros[k];
        const float mv = synthMacroM(values[k], m.bipolar);
        for (int j = 0, c = synthMacroTargets(m); j < c; ++j) {
            const int i = m.targets[j].param;
            if (i < 0 || i >= n) continue;   // empty target (-1) inside count
            const float a = patchBitsToFloat(m.targets[j].amountBits);
            if (!std::isfinite(a)) continue;
            add[i] += a * mv * (t[i].max - t[i].min);
            hit[i] = true;
        }
    }
    for (int i = 0; i < n; ++i)
        if (hit[i]) out[i] = synthParamClamp(p.engine, i, out[i] + add[i]);
    // M7: relative to full velocity (synth_params.h).
    const PatchMacro& m7 = p.macros[kPatchMacroVelocity];
    float mul[kPatchMaxParams];
    for (int i = 0; i < n; ++i) mul[i] = 1.0f;
    bool any = false;
    for (int j = 0, c = synthMacroTargets(m7); j < c; ++j) {
        const int i = m7.targets[j].param;
        if (i < 0 || i >= n) continue;
        const float a = patchBitsToFloat(m7.targets[j].amountBits);
        if (!std::isfinite(a)) continue;
        mul[i] += a * (velocity - 1.0f);
        any = true;
    }
    if (any)
        for (int i = 0; i < n; ++i)
            if (mul[i] != 1.0f) out[i] = synthParamClamp(p.engine, i, out[i] * mul[i]);
    return n;
}

}  // namespace audio
}  // namespace ot

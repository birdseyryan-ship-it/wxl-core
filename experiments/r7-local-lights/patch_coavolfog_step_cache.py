from __future__ import annotations

import argparse
import hashlib
from pathlib import Path
import subprocess


PINNED = "4e31ddf53a326ba9c0ad03488b4c30f7ce23c969"
PAYLOAD = Path(__file__).resolve().parent / "payload"
REQUIRED = {
    "src/msaa_depth.h": "144488a4dd6d0b81baaef8039aa4536b5b06552b6b39cfe976b2cffe01bc75ba",
    "src/msaa_depth.cpp": "4889011b75de1d3461c75a696cb0f063b54f02a32e75d596cdfad2ed486d2708",
    "src/d3d9_wrap.cpp": "d8ce83260e9399c3189f0d1d3dd65eaf878035fd44bff05e9229f9cd99237760",
    "shaders/vf_integrate.hlsli": "9f650e1f61e55c5c9a5cd89303bd579b6421a3060fbec6217f6aab2bdc4f3218",
    "shaders/vf_march.hlsl": "0870874736861def5532c535d1e8efd777e3ae55e28201e336f489abf4ad2d00",
    "CMakeLists.txt": "1ca594e97d363b57d82e67323d56d4338daa46c52504c8d9de396aa30f6efa67",
    "tests/harness.cpp": "8cde33d399278c17f87fd971701919866ae74104521ed5f6e4532e05e041b591",
    "tests/performance_scene.h": "188053f1f9458b2c8de739f31b6f1f0d076d2ec042e3193428f5a601ca311432",
}


def replace_once(value: str, old: str, new: str) -> str:
    if value.count(old) != 1:
        raise ValueError(f"expected one patch site, found {value.count(old)}: {old[:65]}")
    return value.replace(old, new, 1)


def prepare(root: Path, variant: str) -> dict[Path, bytes]:
    head = subprocess.check_output(["git", "-C", str(root), "rev-parse", "HEAD"], text=True).strip()
    top = subprocess.check_output(["git", "-C", str(root), "rev-parse", "--show-toplevel"], text=True).strip()
    if head != PINNED or Path(top).resolve() != root:
        raise ValueError("the target must be the pinned CoAVolFog repository root")
    for name, expected in REQUIRED.items():
        if hashlib.sha256((root / name).read_bytes()).hexdigest() != expected:
            raise ValueError(f"source identity mismatch: {name}; apply only the frozen WXL R6 patch first")
    additions = {
        root / "tests/r7_step_cache_checks.h": (PAYLOAD / "r7_step_cache_checks.h").read_bytes(),
        root / "tests/shaders/r7_reference_march.hlsl":
            b'#define WXL_R7_STEP_CACHE 0\n#include "../../shaders/vf_march.hlsl"\n',
    }
    if any(path.exists() for path in additions):
        raise ValueError("refusing to overwrite an existing R7 harness payload")
    integrated = (root / "shaders/vf_integrate.hlsli").read_text(encoding="utf-8")
    integrated = replace_once(integrated, "#ifndef STEPS\n", "#ifndef WXL_R7_STEP_CACHE\n#define WXL_R7_STEP_CACHE "
                              + ("1" if variant == "cached" else "0") + "\n#endif\n\n#ifndef STEPS\n")
    integrated = replace_once(integrated, "kUnrollsLayers && sampleDistance == step.distance",
                              "(kUnrollsLayers || WXL_R7_STEP_CACHE) && sampleDistance == step.distance")
    cmake = (root / "CMakeLists.txt").read_text(encoding="utf-8")
    references = "".join(
        f"vf_shader(ps_r7_reference_{noise}{quality} ../tests/shaders/r7_reference_march.hlsl ps_3_0 "
        f"/DSTEPS={steps} /DLOCAL_LIGHTS=1" + ("\n          /DSAMPLES_AUTHORED_NOISE=1" if noise else "") + ")\n"
        for noise in ("", "noisy_") for quality, steps in (("low", 16), ("mid", 24), ("high", 32)))
    cmake = replace_once(cmake, "vf_shader(ps_density_probe", references + "vf_shader(ps_density_probe")
    harness = (root / "tests/harness.cpp").read_text(encoding="utf-8")
    harness = replace_once(harness, '#include "authored_noise_checks.h"',
                            '#include "authored_noise_checks.h"\n#include "r7_step_cache_checks.h"')
    harness = replace_once(harness, "    local_light_gpu::CheckLocalLightIntegration(h.dev);",
                            "    local_light_gpu::CheckLocalLightIntegration(h.dev);\n"
                            "    r7_step_cache::CheckMatchesReference(h.dev);")
    harness = replace_once(harness, '    if (scene == L"performance")\n',
                            '    if (scene == L"performance4k")\n        return RunPerformance(samples, 3840, 2160);\n'
                            '    if (scene == L"performance")\n')
    harness = replace_once(harness, "known: harbour, performance, ripples", "known: harbour, performance, performance4k, ripples")
    performance = (root / "tests/performance_scene.h").read_text(encoding="utf-8")
    performance = replace_once(performance, '    std::printf("1080p fog benchmark: %s; %s\\n", adapter.Description, timer.Method());',
        '    std::printf("%ux%u fog benchmark: %s; %s\\n", h.pp.BackBufferWidth, h.pp.BackBufferHeight,\n'
        '                adapter.Description, timer.Method());')
    performance = replace_once(performance,
        "    EngineGlDepthProjection(1.0f / std::tan(kFovY * 0.5f), 1920.0f / 1080.0f, kNear, kFar, projection);",
        "    EngineGlDepthProjection(1.0f / std::tan(kFovY * 0.5f),\n"
        "                            static_cast<float>(h.pp.BackBufferWidth) / h.pp.BackBufferHeight,\n"
        "                            kNear, kFar, projection);")
    performance = replace_once(performance, "{0, 0, 1920, 1080, 0, kClientWorldMaxZ}",
                               "{0, 0, h.pp.BackBufferWidth, h.pp.BackBufferHeight, 0, kClientWorldMaxZ}")
    performance = replace_once(performance, "int RunPerformance(D3DMULTISAMPLE_TYPE samples)",
                               "int RunPerformance(D3DMULTISAMPLE_TYPE samples, UINT width = 1920, UINT height = 1080)")
    performance = replace_once(performance, "0, 0, 1920, 1080, nullptr, nullptr, windowClass.hInstance, nullptr);",
                               "0, 0, static_cast<int>(width), static_cast<int>(height),\n"
                               "                             nullptr, nullptr, windowClass.hInstance, nullptr);")
    performance = replace_once(performance, "h.pp.BackBufferWidth = 1920;", "h.pp.BackBufferWidth = width;")
    performance = replace_once(performance, "h.pp.BackBufferHeight = 1080;", "h.pp.BackBufferHeight = height;")
    changes = {"shaders/vf_integrate.hlsli": integrated, "CMakeLists.txt": cmake,
               "tests/harness.cpp": harness, "tests/performance_scene.h": performance}
    additions.update({root / name: text.encode("utf-8") for name, text in changes.items()})
    return additions


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("root", type=Path)
    parser.add_argument("--variant", choices=("baseline", "cached"), required=True)
    args = parser.parse_args()
    root = args.root.resolve()
    for path, data in prepare(root, args.variant).items():
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(data)
    print(f"PASS: prepared {args.variant}; WXL R6 depth/water files remain byte-identical")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

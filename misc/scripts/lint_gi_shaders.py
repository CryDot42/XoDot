#!/usr/bin/env python3
"""
Standalone syntax/semantic check for the experimental SDFGI Radiance
Cascades + screen probes shaders, without needing a full engine build.

Reproduces, in Python, the two preprocessing steps Godot itself performs
before handing '#[compute]' shader text to glslang:
  1. glsl_builders.py's #include inlining (build_rd_header).
  2. RenderingDevice's per-variant #VERSION_DEFINES substitution (the
     "defines" string + per-mode #define, exactly as gi.cpp passes them
     in initialize_shaders()).

Then compiles every declared variant with the real glslangValidator,
under Vulkan semantics (matching RenderingDevice's Vulkan backend), so
this is a real compile check, not a guess. Requires glslangValidator
(Debian/Ubuntu: `apt-get install glslang-tools`).

Usage: python3 misc/scripts/lint_gi_shaders.py
"""
import os
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
SHADER_DIR = os.path.join(ROOT, "servers", "rendering", "renderer_rd", "shaders")

# (path relative to SHADER_DIR, global defines, [mode defines])
CASES = [
    (
        "environment/sdfgi_integrate.glsl",  # regression check: known-good legacy shader
        ["#define OCT_SIZE 6", "#define SH_SIZE 16"],
        ["#define MODE_PROCESS", "#define MODE_STORE", "#define MODE_SCROLL", "#define MODE_SCROLL_STORE"],
    ),
    (
        "environment/sdfgi_radiance_cascades.glsl",
        ["#define OCT_SIZE 6", "#define RC_MAX_OCT_SIZE 8"],
        ["#define MODE_TRACE", "#define MODE_MERGE", "#define MODE_PROJECT", "#define MODE_STORE"],
    ),
    (
        "environment/sdfgi_radiance_cascades.glsl",  # wider octahedral cap + sky octmap array variant
        ["#define OCT_SIZE 6", "#define RC_MAX_OCT_SIZE 16", "#define USE_RADIANCE_OCTMAP_ARRAY"],
        ["#define MODE_TRACE", "#define MODE_MERGE", "#define MODE_PROJECT", "#define MODE_STORE"],
    ),
    (
        "environment/sdfgi_screen_probes.glsl",
        ["#define SDFGI_OCT_SIZE 6"],
        ["#define MODE_TRACE", "#define MODE_FILTER"],
    ),
    (
        "environment/gi.glsl",  # regression check: existing variants must still compile
        ["#define SDFGI_OCT_SIZE 6"],
        [
            "#define USE_SDFGI",
            "#define USE_VOXEL_GI_INSTANCES",
            "#define USE_VOXEL_GI_INSTANCES\n#define USE_SCREEN_PROBES",
            "#define USE_SDFGI\n#define USE_VOXEL_GI_INSTANCES",
        ],
    ),
    (
        "environment/gi.glsl",  # new variant: SDFGI's own USE_SCREEN_PROBES composite block
        ["#define SDFGI_OCT_SIZE 6"],
        ["#define USE_SDFGI\n#define USE_SCREEN_PROBES"],
    ),
]


def inline_includes(path: str) -> str:
    out_lines = []
    directory = os.path.dirname(path)
    with open(path, "r", encoding="utf-8") as f:
        for line in f:
            stripped = line.strip()
            if stripped.startswith("#[") and stripped.endswith("]"):
                continue  # stage tag (#[compute]), single-stage file
            if stripped.startswith("#include "):
                inc = stripped[len("#include "):].strip()[1:-1]
                inc_path = os.path.relpath(inc) if inc.startswith("thirdparty/") else os.path.normpath(os.path.join(directory, inc))
                out_lines.append(inline_includes(inc_path))
                continue
            out_lines.append(line)
    return "".join(out_lines)


def compile_variant(glsl_text: str) -> tuple[bool, str]:
    with tempfile.NamedTemporaryFile("w", suffix=".comp", delete=False, encoding="utf-8") as f:
        f.write(glsl_text)
        src_path = f.name
    spv_path = src_path + ".spv"
    try:
        proc = subprocess.run(
            ["glslangValidator", "--target-env", "vulkan1.1", "-V", "-S", "comp", src_path, "-o", spv_path],
            capture_output=True,
            text=True,
        )
        return proc.returncode == 0, proc.stdout + proc.stderr
    finally:
        for p in (src_path, spv_path):
            if os.path.exists(p):
                os.remove(p)


def main() -> int:
    if subprocess.run(["which", "glslangValidator"], capture_output=True).returncode != 0:
        print("glslangValidator not found (apt-get install glslang-tools). Cannot lint.", file=sys.stderr)
        return 2

    all_ok = True
    for rel_path, defines, modes in CASES:
        path = os.path.join(SHADER_DIR, rel_path)
        base = inline_includes(path)
        for mode in modes:
            variant_src = base.replace("#VERSION_DEFINES", "\n".join(defines + [mode]))
            ok, output = compile_variant(variant_src)
            label = mode.replace("\n", " ").replace("#define ", "")
            print(f"[{'OK' if ok else 'FAIL'}] {rel_path} :: {label}")
            if not ok:
                all_ok = False
                print(output)

    return 0 if all_ok else 1


if __name__ == "__main__":
    sys.exit(main())

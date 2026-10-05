#!/usr/bin/env python3
"""Offline SPIR-V checks only: no GL context, window, or game launch.

Run after `make`. Uses the existing toolchain binaries and exported pure IR
rewriter. Fixtures live in a TemporaryDirectory and are removed after the run.
"""
import ctypes
import pathlib
import struct
import subprocess
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[1]
GLSLANG = ROOT / "external/glslang/build/StandAlone/glslang"
VALIDATOR = ROOT / "external/SPIRV-Tools/build/tools/spirv-val"
CROSS = ROOT / "external/SPIRV-Cross/build/spirv-cross"
U32 = ctypes.c_uint32
lib = ctypes.CDLL(str(ROOT / "build/libmgl.dylib"))
rewrite = lib.mglNativeDepthIR
rewrite.argtypes = [ctypes.POINTER(U32), ctypes.c_size_t, ctypes.POINTER(U32),
                    ctypes.c_size_t, ctypes.c_uint64, ctypes.c_uint64,
                    ctypes.POINTER(ctypes.POINTER(U32)), ctypes.POINTER(ctypes.c_size_t)]
rewrite.restype = ctypes.c_bool
free = ctypes.CDLL(None).free
free.argtypes = [ctypes.c_void_p]


def run(*args):
    return subprocess.run([str(a) for a in args], check=True, capture_output=True, text=True).stdout


def instructions(words):
    at = 5
    while at < len(words):
        n = words[at] >> 16
        yield words[at] & 65535, words[at:at+n]
        at += n


def names(words):
    result = {}
    for op, inst in instructions(words):
        if op == 5:  # OpName
            raw = struct.pack("<" + "I" * (len(inst) - 2), *inst[2:])
            result[raw.split(b"\0", 1)[0].decode()] = inst[1]
    return result


HEADER = """#version 450
layout(binding=0) uniform sampler2D a;
layout(binding=1) uniform sampler2D b;
layout(location=0) in vec2 uv;
layout(location=0) out vec4 color;
"""
CASES = {
    "sample": "color = texture(a, uv) + texture(b, uv);",
    "lod": "color = textureLod(a, uv, 1.0) + texture(b, uv);",
    "grad": "color = textureGrad(a, uv, dFdx(uv), dFdy(uv)) + texture(b, uv);",
    "fetch": "color = texelFetch(a, ivec2(uv), 1) + texture(b, uv);",
    "combined": "color = texture(a,uv) + textureLod(a,uv,1.0) + textureGrad(a,uv,dFdx(uv),dFdy(uv)) + texelFetch(a,ivec2(uv),1) + texture(b,uv);",
    "gather": "color = textureGather(a, uv) + texture(b, uv);",
    "offset": "color = textureOffset(a, uv, ivec2(1,0)) + texture(b, uv);",
    "projection": "color = textureProj(a, vec3(uv, 1.0)) + texture(b, uv);",
    "helper": "color = helper(a, uv) + texture(b, uv);",
    "array": "color = texture(a, vec3(uv, 0.0)) + texture(b, uv);",
    "ms": "color = texelFetch(a, ivec2(uv), 0) + texture(b, uv);",
    "shadow": "color = vec4(texture(a, vec3(uv, 0.5))) + texture(b, uv);",
}
REJECT = {"gather", "offset", "projection", "helper", "array", "ms", "shadow"}

with tempfile.TemporaryDirectory(prefix="mgl-depth-ir-") as directory:
    directory = pathlib.Path(directory)
    for case, body in CASES.items():
        src = directory / (case + ".frag")
        original = directory / (case + ".spv")
        helper = "vec4 helper(sampler2D s, vec2 p) { return texture(s,p); }\n" if case == "helper" else ""
        header = HEADER
        replacement = {"array": "sampler2DArray", "ms": "sampler2DMS", "shadow": "sampler2DShadow"}.get(case)
        if replacement:
            header = header.replace("uniform sampler2D a", f"uniform {replacement} a")
        src.write_text(header + helper + "void main() { " + body + " }\n")
        run(GLSLANG, "-G", "-o", original, src)
        data = original.read_bytes()
        words = list(struct.unpack("<" + "I" * (len(data) // 4), data))
        ids = names(words)
        raw = (U32 * len(words))(*words)
        resource_ids = (U32 * 2)(ids["a"], ids["b"])
        original_vars = {w[2]: w[1] for op, w in instructions(words) if op == 59}
        for depth, flip in ((1, 0), (1, 1), (3, 1), (0, 1)):
            out = ctypes.POINTER(U32)()
            length = ctypes.c_size_t()
            ok = rewrite(raw, len(words), resource_ids, 2, depth, flip, ctypes.byref(out), ctypes.byref(length))
            assert bytes(raw) == data, "source IR mutated"
            if case in REJECT:
                assert not ok and not out and length.value == 0, (case, "unsupported use accepted")
                continue
            assert ok, (case, depth, flip, "rewrite rejected")
            try:
                result = list(out[:length.value])
                target = directory / f"{case}-{depth}-{flip}.spv"
                target.write_bytes(struct.pack("<" + "I" * len(result), *result))
                run(VALIDATOR, target)
                vars_after = {w[2]: w[1] for op, w in instructions(result) if op == 59}
                if not (depth | flip) & 2:
                    assert vars_after[ids["b"]] == original_vars[ids["b"]], "shared sampler type changed"
                msl = run(CROSS, target, "--msl", "--msl-version", "30100")
                if depth:
                    assert "depth2d<float>" in msl, "native depth resource missing"
                if not (depth & 2):
                    assert "texture2d<float>" in msl, "color resource changed"
                if flip:
                    assert "1.0 -" in msl or case == "fetch", "coordinate flip missing"
                metal = target.with_suffix(".metal")
                metal.write_text(msl)
                run("xcrun", "-sdk", "macosx", "metal", "-std=metal3.1", "-c", metal,
                    "-o", target.with_suffix(".air"))
            finally:
                free(out)
        if case not in REJECT:
            # Reflection order need not equal declaration order. Selecting both
            # in reverse order must still emit shared private types before use.
            reversed_ids = (U32 * 2)(ids["b"], ids["a"])
            out = ctypes.POINTER(U32)()
            length = ctypes.c_size_t()
            assert rewrite(raw, len(words), reversed_ids, 2, 3, 3, ctypes.byref(out), ctypes.byref(length))
            free(out)
            # Malformed input and invalid masks must fail transactionally.
            out = ctypes.POINTER(U32)()
            length = ctypes.c_size_t(999)
            assert not rewrite(raw, 4, resource_ids, 2, 1, 0, ctypes.byref(out), ctypes.byref(length))
            assert not out and length.value == 0
            assert not rewrite(raw, len(words), resource_ids, 2, 4, 0, ctypes.byref(out), ctypes.byref(length))
            assert not out and length.value == 0
        print(f"PASS {case}")
print("PASS: validator, source immutability, shared type isolation, operation fallback, MSL lowering and Metal compilation")

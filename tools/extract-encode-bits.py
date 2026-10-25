#!/usr/bin/env python3
"""Extract the actual pinned RADV writer bodies for independent C23 checks."""
import argparse
from pathlib import Path
import re


def extract(source: Path, prefix: str) -> str:
    directory = source / "src/amd/vulkan"
    code = (directory / "radv_video_enc.c").read_text()
    header = (directory / "radv_cmd_buffer.h").read_text()
    begin = header.index("struct radv_enc_state {")
    end = header.index("\n};", begin) + 3
    state = header[begin:end]
    first = "radv_enc_set_emulation_prevention"
    if first not in code:
        first = "radv_enc_code_fixed_bits"
    declaration = re.search(r"(?m)^static [^\n]+\n" + first + r"\(", code)
    if declaration is None:
        raise ValueError(f"writer declaration not found: {source}")
    body = code[declaration.start():code.index("static void\nradv_enc_code_ue")]
    shim = """
struct ac_cmdbuf { uint32_t *buf; unsigned cdw; };
struct radv_cmd_stream { struct ac_cmdbuf *b; };
struct radv_cmd_buffer {
    struct radv_cmd_stream *cs;
    struct { struct radv_enc_state enc; } video;
};
"""
    result = state + shim + body
    result = re.sub(r"\b(radv_\w+|ac_cmdbuf|index_to_shifts)\b",
                    lambda match: prefix + "_" + match[0], result)
    return code[:code.index("#include")] + result


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("control", type=Path)
    parser.add_argument("candidate", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    for name, source in (("control", args.control), ("candidate", args.candidate)):
        (args.output / f"encode_bits_{name}.h").write_text(extract(source, name))


if __name__ == "__main__":
    main()

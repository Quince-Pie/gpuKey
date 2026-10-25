#!/usr/bin/env python3
"""Conditional memory targets for full-frame AV1 recording; not a universal minimum.

The model preserves resolution, bit depth and reference count. It excludes
alignment, compression, firmware/driver scratch, code, stacks and allocator cost.
See docs/memory-targets.md for lifetime assumptions and the accounting boundary.
"""

import argparse
import json
from fractions import Fraction


def positive(text: str) -> int:
    value = int(text)
    if value < 1:
        raise argparse.ArgumentTypeError("expected a positive integer")
    return value


def nonnegative_integer(text: str) -> int:
    value = int(text)
    if value < 0:
        raise argparse.ArgumentTypeError("expected a nonnegative integer")
    return value


def nonnegative(text: str) -> Fraction:
    try:
        value = Fraction(text)
    except (ValueError, ZeroDivisionError) as error:
        raise argparse.ArgumentTypeError("expected a finite nonnegative number") from error
    if value < 0:
        raise argparse.ArgumentTypeError("expected a nonnegative number")
    return value


def ceiling(value: Fraction) -> int:
    return -(-value.numerator // value.denominator)


def targets(width: int, height: int, bits: int, sources: int, depth: int,
            refs: int, inputs: int = 1) -> dict:
    samples = width * height + 2 * ((width + 1) // 2) * ((height + 1) // 2)
    rgb = 4 * width * height
    yuv = samples * (1 if bits == 8 else 2)
    recon = (refs + 1) * yuv
    source = sources * rgb
    # The pinned VAAPI encoder aligns AV1 coded dimensions to 64 by default.
    coded_width, coded_height = (width + 63) // 64 * 64, (height + 63) // 64 * 64
    coded_buffer = 3 * coded_width * coded_height + 65536
    return {
        "assumptions": {"width": width, "height": height, "bits": bits,
                        "source_buffers": sources, "encode_depth_capacity": depth,
                        "distinct_materialized_inputs": inputs,
                        "past_reference_images": refs},
        "per_image_bytes": {"rgbx8": rgb, "native_nv12_or_p010": yuv,
                            "packed_sample_bits_div_8": ceiling(Fraction(samples * bits, 8))},
        "conditional_vram_payload_targets": {
            "shared_capture": source,
            "reconstruction_with_ordered_reuse": recon,
            "native_converted_inputs": inputs * yuv,
            "native_private": (inputs + refs + 1) * yuv,
            "native_pipeline_union": source + (inputs + refs + 1) * yuv,
            "one_input_pipeline_union": source + (refs + 2) * yuv,
            "inputs_at_full_depth_pipeline_union": source + (depth + refs + 1) * yuv,
            "direct_rgb_pipeline_union_same_source_count": source + recon,
            "producer_yuv_pipeline_union_same_source_count": (sources + refs + 1) * yuv,
            "distinct_inflight_reconstructions_pipeline_union": source + (inputs + depth + refs) * yuv,
        },
        "conditional_direct_rgb_standby_targets": {
            "requires": "independent key, completed GPU work, no pending dependent picture",
            "live_reconstruction_images": 1,
            "pipeline_union_without_unused_reconstructions": source + yuv,
            "pipeline_union_with_one_unused_reconstruction": source + 2 * yuv,
            "excludes": "alignment, driver workspace, coded buffers, CPU key payload; resume may allocate",
        },
        "conditional_vaapi_standby_target": {
            "requires": "validated CPU key owns independent refreshes; all input reads completed",
            "materialized_converted_inputs": 0,
            "retained_reconstruction_capacity": refs + 1,
            "pipeline_union_bytes": source + recon,
            "excludes": "alignment, driver storage, coded buffers and CPU key metadata; resume may allocate",
        },
        "conditional_vaapi_surface_owned_standby_target": {
            "requires": "validated independent CPU key, completed GPU reads, surface-owned reusable DPB",
            "materialized_converted_inputs": 0,
            "live_reconstruction_images": 1,
            "pipeline_union_bytes": source + yuv,
            "excludes": "alignment, driver workspace, coded buffers and CPU key metadata; resume may allocate",
        },
        "conditional_uploaded_gpu_targets": {
            "requires": "CPU conversion/upload path; source buffers remain in host RAM",
            "active_one_input_vram_bytes": (1 + refs + 1) * yuv,
            "active_two_input_vram_bytes": (2 + refs + 1) * yuv,
            "active_cpu_staging_bytes": yuv,
            "standby_vram_with_reclaimed_reconstructions_bytes": yuv,
            "standby_vram_with_retained_reconstructions_bytes": recon,
            "standby_cpu_staging_bytes": 0,
            "standby_gtt_staging_bytes": 0,
            "excludes": "alignment, driver workspace, coded buffers, optional conversion row cache and CPU key metadata; resume may allocate",
        },
        "pinned_ffmpeg_reservations_not_lower_bounds": {
            "coded_width": coded_width, "coded_height": coded_height,
            "coded_buffer_bytes_each": coded_buffer,
            "coded_buffers_with_completed_output_reuse_bytes": coded_buffer,
            "coded_buffers_at_encode_depth_bytes": depth * coded_buffer,
        },
        "host_raw_pixel_copy_target_bytes": 0,
    }


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--width", type=positive, default=2560)
    parser.add_argument("--height", type=positive, default=2880)
    parser.add_argument("--bits", type=int, choices=[8, 10], default=8)
    parser.add_argument("--source-buffers", type=positive, default=2)
    parser.add_argument("--encode-depth", type=positive, default=2)
    parser.add_argument("--inputs", type=nonnegative_integer, default=1,
                        help="separate converted inputs; zero for direct RGB (default: 1)")
    parser.add_argument("--references", type=nonnegative_integer, default=2)
    parser.add_argument("--replay-seconds", type=nonnegative, default=Fraction(30))
    parser.add_argument("--preroll-seconds", type=nonnegative, default=Fraction(0),
                        help="actual/assumed preceding-keyframe interval, not a guaranteed GOP bound")
    parser.add_argument("--bitrate-mbps", type=nonnegative,
                        help="average aggregate encoded payload rate over this window, decimal Mbit/s")
    args = parser.parse_args()
    result = targets(args.width, args.height, args.bits, args.source_buffers,
                     args.encode_depth, args.references, args.inputs)
    window = args.replay_seconds + args.preroll_seconds
    payload = (ceiling(args.bitrate_mbps * 1000000 * window / 8)
               if args.bitrate_mbps is not None else None)
    result["replay_ram_payload_target"] = {
        "model": "one stored copy per original packet; exact interning/compression may use less",
        "window_seconds": str(window),
        "aggregate_bitrate_mbps": str(args.bitrate_mbps) if args.bitrate_mbps is not None else None,
        "encoded_payload_bytes": payload,
        "formula": "sum of retained encoded packet sizes; bitrate * (history + actual preroll) / 8",
        "excludes": "packet metadata, codec configuration, audio state, export-pinned old data, allocator and runtime",
    }
    print(json.dumps(result, indent=2))


if __name__ == "__main__":
    main()

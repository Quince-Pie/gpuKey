WAYLAND GPU RECORDER - DESIGN COMPANION
Research snapshot: 19 October 2026

This bundle contains the reviewed design source, the primary-source registry,
three executed abstract design checks, their actual JSON results, and the
scripts used to build and inspect the companion PDF.

The main deliverable is the separately supplied Wayland_GPU_Recorder_Design.pdf:
61 pages; approximately 22,000 body words; 108 source identifiers.

START HERE
  research/Wayland_GPU_Recorder_Design.md  Complete readable design source
  research/source_registry.json           Exact source IDs and primary URLs
  research/verification_summary.json      Actual checks and important limits

REPRODUCE THE ABSTRACT CHECKS
From the extracted bundle root, with Python 3.12 (tested):
  python3 recorder_concurrency_model_check.py
  python3 research/check_design_arithmetic.py
  python3 research/check_output_model.py

The checks use the Python standard library. They deliberately include broken
controls and assert the expected counterexamples. A counterexample in a broken
control is an expected validation result, not a failing delivered design.

The concurrency model uses finite sequentially consistent abstract schedules.
The damage model is a scalar 4x4 source and 2x2 box filter with three destinations.
The output model covers finite positive short completions of two/three
nonoverlapping writes and a small independent cancellation model. Consult each
script and JSON scope before generalizing. None executes GPU or io_uring work.

REBUILD THE DOCUMENT (OPTIONAL)
Install the versions in requirements-document.txt and DejaVu fonts, then run:
  python3 research/build_design.py
  python3 research/verify_design_pdf.py

Set RECORDER_DESIGN_FONT_DIR if DejaVu is not in a detected system location.
The visual QA script also expects a system DejaVuSans.ttf for contact labels.
The build writes output/pdf/Wayland_GPU_Recorder_Design.pdf and renders into
scratch-like tmp/pdfs. Re-rendering does not by itself constitute visual review.

CONTENTS AND AUTHORITY
The chapter files are the reviewed build inputs. The combined source is the
readable reference. The source registry links primary material; proprietary
SDKs, third-party paper PDFs and vendor source downloads are not redistributed.
The requirement brief labels explicit requirements and provisional choices.

No recorder implementation, native C23 proof, GPU performance comparison,
codec-quality ranking, storage benchmark or player-compatibility result is
claimed. Actual hardware probes remain the acceptance gates documented in the
report. C23 is the intended recorder host language; Python is used only for
these design checks and document generation.

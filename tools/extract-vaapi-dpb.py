#!/usr/bin/env python3
"""Extract actual Mesa AV1 ownership operations for a GPU-free lifetime oracle."""

import argparse
from pathlib import Path
import re
import shutil
import subprocess
import tempfile


def region(text: str, start: str, end: str) -> str:
    first = text.index(start)
    last = text.index(end, first) + len(end)
    return text[first:last]


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('mesa', type=Path)
    parser.add_argument('output', type=Path)
    parser.add_argument('--patch', type=Path)
    args = parser.parse_args()
    source = args.mesa
    with tempfile.TemporaryDirectory() as temporary:
        if args.patch:
            source = Path(temporary)
            for name in ('frontends/va/picture_av1_enc.c', 'frontends/va/surface.c',
                         'frontends/va/context.c', 'frontends/va/va_private.h',
                         'drivers/radeonsi/mm/radeon_vcn_enc.c'):
                relative = Path('src/gallium') / name
                target = source / relative
                target.parent.mkdir(parents=True, exist_ok=True)
                shutil.copyfile(args.mesa / relative, target)
            subprocess.run(['patch', '--batch', '-p1', '-i', str(args.patch.resolve())],
                           cwd=source, check=True, stdout=subprocess.DEVNULL)
        types = (args.mesa / 'src/gallium/include/pipe/p_video_state.h').read_text()
        assert re.search(r'#define PIPE_AV1_MAX_DPB_SIZE\s+8\b', types)
        assert re.search(r'struct pipe_av1_enc_dpb_entry\s*\{\s*uint32_t id;\s*'
                         r'uint32_t order_hint;\s*struct pipe_video_buffer \*buffer;\s*\};', types)
        picture = (source / 'src/gallium/frontends/va/picture_av1_enc.c').read_text()
        start = ('   surf = handle_table_get(drv->htab, av1->reconstructed_frame);'
                 if args.patch else '   /* Evict unused surfaces */')
        body = region(picture, start,
                      '   context->desc.av1enc.dpb[i].buffer = surf->buffer;')
        surface = (source / 'src/gallium/frontends/va/surface.c').read_text()
        removal = region(surface, '   case PIPE_VIDEO_FORMAT_AV1:', '\n      break;')
        removal = removal.split(':', 1)[1].rsplit('break;', 1)[0]
        assert removal.count('{') == removal.count('}')
        context = (source / 'src/gallium/frontends/va/context.c').read_text()
        cleanup = region(context, '         for (uint32_t i = 0; i < ARRAY_SIZE(context->desc.av1enc.dpb); i++) {',
                         '         util_dynarray_fini(&context->desc.av1enc.raw_headers);')
        cleanup = cleanup.rsplit('         util_dynarray_fini', 1)[0]
        assert '   int i, j;' in picture
        args.output.write_text('''/* Generated from the actual library sources. Preserve the library's
 * signed loop indices and byte-sized fields; its normal flags permit their
 * checked small-index conversions. Other warnings, including uninitialized
 * access, remain enabled. */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wconversion"
static int apply(vlVaDriver *drv, vlVaContext *context, const struct picture *av1)
{
   int i, j;
   vlVaSurface *surf;
''' + body + '''
   return VA_STATUS_SUCCESS;
}
static void remove_surface(vlVaSurface *surf, uint32_t id)
{
''' + removal + '''
}
static void release_cached(vlVaContext *context)
{
''' + cleanup + '\n}\n#pragma GCC diagnostic pop\n')


if __name__ == '__main__':
    main()

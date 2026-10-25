#!/usr/bin/env python3
"""Extract actual Vulkan transfer and media upload-pool operations for a GPU-free oracle."""

import argparse
from pathlib import Path
import shutil
import subprocess
import tempfile


def region(text: str, start: str, end: str) -> str:
    first = text.index(start)
    last = text.index(end, first) + len(end)
    return text[first:last]


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('ffmpeg', type=Path)
    parser.add_argument('media', type=Path)
    parser.add_argument('output', type=Path)
    parser.add_argument('--patch', type=Path)
    args = parser.parse_args()
    source = args.ffmpeg
    with tempfile.TemporaryDirectory() as temporary:
        if args.patch:
            source = Path(temporary)
            relative = Path('libavutil/hwcontext_vulkan.c')
            target = source / relative
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(args.ffmpeg / relative, target)
            for prerequisite in ('nix/ffmpeg-vulkan-buffer-owner.patch',
                                 'nix/ffmpeg-pool-trim.patch',
                                 'nix/ffmpeg-vulkan-rgb.patch'):
                subprocess.run(['patch', '--batch', '-p1', '--forward', '-i',
                                str(Path(prerequisite).resolve())],
                               cwd=source, check=False, stdout=subprocess.DEVNULL)
            subprocess.run(['patch', '--batch', '-p1', '-i', str(args.patch.resolve())],
                           cwd=source, check=True, stdout=subprocess.DEVNULL)
        vulkan = (source / 'libavutil/hwcontext_vulkan.c').read_text()
        free_cb = region(vulkan, 'static void vulkan_frame_free_cb(void *opaque, uint8_t *data)\n{', '\n}\n')
        transfer_fn = region(
            vulkan,
            'static int vulkan_transfer_frame(AVHWFramesContext *hwfc,',
            '    return err;\n}'
        )
        transfer_deps = region(
            transfer_fn,
            '    /* Prep destination Vulkan frame */',
            '    ff_vk_frame_barrier(&p->vkctx, exec, hwf, img_bar, &nb_img_bar,'
        ).rsplit('\n    ff_vk_frame_barrier', 1)[0]
        transfer_tail = region(
            transfer_fn,
            '    err = ff_vk_exec_submit(&p->vkctx, exec);',
            '    return err;\n}'
        )
        media = args.media.read_text()
        ensure = region(media, 'static int ensure_staging(struct gk_media *m)\n{', '\n}\n')
        idle = region(media, 'static void release_idle_inputs(struct gk_media *m)\n{', '\n}\n')
        standby = region(media, 'static int release_static_resources(struct gk_media *m)\n{', '\n}\n')
        args.output.write_text('''/* Generated from actual FFmpeg and gpuKey sources. */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wconversion"
''' + free_cb + '''
static int run_transfer_tail(AVHWFramesContext *hwfc, FFVkExecContext *exec,
                             AVFrame *hwf, AVFrame *swf, AVBufferRef **bufs,
                             int nb_bufs, int upload, int host_mapped,
                             int planes, VkBufferImageCopy *region)
{
    VulkanDevicePriv *p = hwfc->device_ctx->hwctx;
    int err = 0;
''' + transfer_deps + '\n' + transfer_tail + '\n' + ensure + '\n' + idle + '\n' + standby + '''
#pragma GCC diagnostic pop
''')


if __name__ == '__main__':
    main()

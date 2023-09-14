#
# Copyright 2023 Matthieu Bouron <matthieu.bouron@gmail.com>
# Copyright 2023 Clément Bœsch <u pkh.me>
# Copyright 2023 Nope Foundry
# Copyright 2017-2022 GoPro Inc.
#
# Licensed to the Apache Software Foundation (ASF) under one
# or more contributor license agreements.  See the NOTICE file
# distributed with this work for additional information
# regarding copyright ownership.  The ASF licenses this file
# to you under the Apache License, Version 2.0 (the
# "License"); you may not use this file except in compliance
# with the License.  You may obtain a copy of the License at
#
#   http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing,
# software distributed under the License is distributed on an
# "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
# KIND, either express or implied.  See the License for the
# specific language governing permissions and limitations
# under the License.
#

import os
import platform
import subprocess
import sys
from dataclasses import dataclass
from typing import List, Optional

from pynopegl_utils.misc import SceneInfo, get_backend, get_viewport

import pynopegl as ngl

RESOLUTIONS = {
    "144p": 144,
    "240p": 240,
    "360p": 360,
    "480p": 480,
    "720p": 720,
    "1080p": 1080,
    "1440p": 1440,
    "4k": 2048,
}


@dataclass
class EncodeProfile:
    name: str
    format: str
    args: List[str]


ENCODE_PROFILES = dict(
    mp4_h264_420=EncodeProfile(
        name="MP4 / H264 4:2:0",
        format="mp4",
        # Since 4:2:0 is used for portability (over the Internet typically), we also use faststart
        args=["-pix_fmt", "yuv420p", "-c:v", "libx264", "-crf", "18", "-movflags", "+faststart"],
    ),
    mp4_h264_444=EncodeProfile(
        name="MP4 / H264 4:4:4",
        format="mp4",
        args=["-pix_fmt", "yuv444p", "-c:v", "libx264", "-crf", "18"],
    ),
    mov_qtrle=EncodeProfile(
        name="MOV / QTRLE (Lossless)",
        format="mov",
        args=["-c:v", "qtrle"],
    ),
    nut_ffv1=EncodeProfile(
        name="NUT / FFV1 (Lossless)",
        format="nut",
        args=["-c:v", "ffv1"],
    ),
)


def export_worker(
    scene_info: SceneInfo,
    filename: str,
    resolution: str,
    extra_enc_args: Optional[List[str]] = None,
):
    scene = scene_info.scene
    fps = scene.framerate
    duration = scene.duration
    samples = scene_info.samples

    ar = scene.aspect_ratio
    height = RESOLUTIONS[resolution]
    width = int(height * ar[0] / ar[1])
    width &= ~1  # make sure it's a multiple of 2 for the h264 codec

    fd_r, fd_w = os.pipe()

    ffmpeg = ["ffmpeg"]
    input = f"pipe:{fd_r}"

    if platform.system() == "Windows":
        import msvcrt

        handle = msvcrt.get_osfhandle(fd_r)
        os.set_handle_inheritable(handle, True)
        input = f"handle:{handle}"
        ffmpeg = [sys.executable, "-m", "pynopegl_utils.viewer.ffmpeg_win32"]

    # fmt: off
    cmd = ffmpeg + [
        "-r", "%d/%d" % fps,
        "-v", "warning",
        "-nostats", "-nostdin",
        "-f", "rawvideo",
        "-video_size", "%dx%d" % (width, height),
        "-pixel_format", "rgba",
        "-i", input,
    ]
    # fmt: on
    if extra_enc_args:
        cmd += extra_enc_args
    cmd += ["-y", filename]

    if platform.system() == "Windows":
        reader = subprocess.Popen(cmd, close_fds=False)
    else:
        reader = subprocess.Popen(cmd, pass_fds=(fd_r,))
    os.close(fd_r)

    capture_buffer = bytearray(width * height * 4)

    ctx = ngl.Context()
    ctx.configure(
        ngl.Config(
            platform=ngl.Platform.AUTO,
            backend=get_backend(scene_info.backend),
            offscreen=True,
            width=width,
            height=height,
            viewport=get_viewport(width, height, scene.aspect_ratio),
            samples=samples,
            clear_color=scene_info.clear_color,
            capture_buffer=capture_buffer,
        )
    )
    ctx.set_scene(scene)

    # Draw every frame
    nb_frame = int(duration * fps[0] / fps[1])
    for i in range(nb_frame):
        time = i * fps[1] / float(fps[0])
        ctx.draw(time)
        os.write(fd_w, capture_buffer)
        yield i * 100 / nb_frame
    yield 100

    os.close(fd_w)
    reader.wait()

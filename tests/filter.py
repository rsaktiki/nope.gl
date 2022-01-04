#
# Copyright 2021 GoPro Inc.
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

import pynodegl as ngl
from pynodegl_utils.misc import scene
from pynodegl_utils.tests.cmp_cuepoints import test_cuepoints


_CUEPOINTS = dict(c=(0, 0), bl=(-.5, -.5), br=(.5, -.5), tr=(.5, .5), tl=(-.5, .5))


def _base_scene(cfg, *filters):
    return ngl.RenderGradient4(
        opacity_tl=0.3,
        opacity_tr=0.4,
        opacity_br=0.5,
        opacity_bl=0.6,
        filters=filters,
    )


@test_cuepoints(points=_CUEPOINTS, nb_keyframes=1, tolerance=1)
@scene()
def filter_alpha(cfg):
    return _base_scene(cfg, ngl.FilterAlpha(0.4321))


@test_cuepoints(points=_CUEPOINTS, nb_keyframes=1, tolerance=1)
@scene()
def filter_contrast(cfg):
    return _base_scene(cfg, ngl.FilterContrast(1.2, pivot=0.3))


@test_cuepoints(points=_CUEPOINTS, nb_keyframes=1, tolerance=1)
@scene()
def filter_exposure(cfg):
    return _base_scene(cfg, ngl.FilterExposure(0.7))


@test_cuepoints(points=_CUEPOINTS, nb_keyframes=1, tolerance=1)
@scene()
def filter_inversealpha(cfg):
    return _base_scene(cfg, ngl.FilterInverseAlpha())


@test_cuepoints(points=_CUEPOINTS, nb_keyframes=1, tolerance=1)
@scene()
def filter_opacity(cfg):
    return _base_scene(cfg, ngl.FilterOpacity(0.4321))


@test_cuepoints(points=_CUEPOINTS, nb_keyframes=1, tolerance=1)
@scene()
def filter_saturation(cfg):
    return _base_scene(cfg, ngl.FilterSaturation(1.5))


@test_cuepoints(points=_CUEPOINTS, nb_keyframes=1, tolerance=1)
@scene()
def filter_composition_colors(cfg):
    return ngl.RenderGradient4(
        filters=(
            ngl.FilterExposure(exposure=0.9),
            ngl.FilterContrast(contrast=1.5),
            ngl.FilterSaturation(saturation=1.1),
            ngl.FilterOpacity(opacity=0.8),
        ),
    )


@test_cuepoints(points=_CUEPOINTS, nb_keyframes=1, tolerance=1)
@scene()
def filter_composition_alpha(cfg):
    return ngl.RenderGradient(
        color0=(1, 0.5, 0),
        color1=(0, 1, 0.5),
        opacity0=0.8,
        opacity1=0.9,
        mode='radial',
        filters=(
            ngl.FilterAlpha(alpha=1.0),
            ngl.FilterInverseAlpha(),
            ngl.FilterAlpha(alpha=0.1),
            ngl.FilterInverseAlpha(),
            ngl.FilterPremult(),
        )
    )

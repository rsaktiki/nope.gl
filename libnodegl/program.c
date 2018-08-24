/*
 * Copyright 2018 GoPro Inc.
 *
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 * KIND, either express or implied.  See the License for the
 * specific language governing permissions and limitations
 * under the License.
 */

#include <stdlib.h>
#include <string.h>

#include "glincludes.h"
#include "log.h"
#include "nodes.h"
#include "program.h"

int ngli_program_check_status(const struct glcontext *gl, GLuint id, GLenum status)
{
    char *info_log = NULL;
    int info_log_length = 0;

    void (*get_info)(const struct glcontext *gl, GLuint id, GLenum pname, GLint *params);
    void (*get_log)(const struct glcontext *gl, GLuint id, GLsizei bufSize, GLsizei *length, GLchar *infoLog);
    const char *type_str;

    if (status == GL_COMPILE_STATUS) {
        type_str = "compile";
        get_info = ngli_glGetShaderiv;
        get_log  = ngli_glGetShaderInfoLog;
    } else if (status == GL_LINK_STATUS) {
        type_str = "link";
        get_info = ngli_glGetProgramiv;
        get_log  = ngli_glGetProgramInfoLog;
    } else {
        ngli_assert(0);
    }

    GLint result = GL_FALSE;
    get_info(gl, id, status, &result);
    if (result == GL_TRUE)
        return 0;

    get_info(gl, id, GL_INFO_LOG_LENGTH, &info_log_length);
    if (!info_log_length)
        return -1;

    info_log = malloc(info_log_length);
    if (!info_log)
        return -1;

    get_log(gl, id, info_log_length, NULL, info_log);
    while (info_log_length && strchr(" \r\n", info_log[info_log_length - 1]))
        info_log[--info_log_length] = 0;

    LOG(ERROR, "could not %s shader: %s", type_str, info_log);
    return -1;
}

static void free_pinfo(void *user_arg, void *data)
{
    free(data);
}

struct hmap *ngli_program_probe_uniforms(const char *node_name, struct glcontext *gl, GLuint pid)
{
    struct hmap *umap = ngli_hmap_create();
    if (!umap)
        return NULL;
    ngli_hmap_set_free(umap, free_pinfo, NULL);

    int nb_active_uniforms;
    ngli_glGetProgramiv(gl, pid, GL_ACTIVE_UNIFORMS, &nb_active_uniforms);
    for (int i = 0; i < nb_active_uniforms; i++) {
        char name[MAX_ID_LEN];
        struct uniformprograminfo *info = malloc(sizeof(*info));
        if (!info) {
            ngli_hmap_freep(&umap);
            return NULL;
        }
        ngli_glGetActiveUniform(gl, pid, i, sizeof(name), NULL,
                                &info->size, &info->type, name);

        /* Remove [0] suffix from names of uniform arrays */
        name[strcspn(name, "[")] = 0;
        info->id = ngli_glGetUniformLocation(gl, pid, name);

        if (info->type == GL_IMAGE_2D) {
            ngli_glGetUniformiv(gl, pid, info->id, &info->binding);
        } else {
            info->binding = -1;
        }

        LOG(DEBUG, "%s.uniform[%d/%d]: %s location:%d size=%d type=0x%x binding=%d", node_name,
            i + 1, nb_active_uniforms, name, info->id, info->size, info->type, info->binding);

        int ret = ngli_hmap_set(umap, name, info);
        if (ret < 0) {
            ngli_hmap_freep(&umap);
            return NULL;
        }
    }

    return umap;
}

struct hmap *ngli_program_probe_attributes(const char *node_name, struct glcontext *gl, GLuint pid)
{
    struct hmap *amap = ngli_hmap_create();
    if (!amap)
        return NULL;
    ngli_hmap_set_free(amap, free_pinfo, NULL);

    int nb_active_attributes;
    ngli_glGetProgramiv(gl, pid, GL_ACTIVE_ATTRIBUTES, &nb_active_attributes);
    for (int i = 0; i < nb_active_attributes; i++) {
        char name[MAX_ID_LEN];
        struct attributeprograminfo *info = malloc(sizeof(*info));
        if (!info) {
            ngli_hmap_freep(&amap);
            return NULL;
        }
        ngli_glGetActiveAttrib(gl, pid, i, sizeof(name), NULL,
                               &info->size, &info->type, name);

        info->id = ngli_glGetAttribLocation(gl, pid, name);
        LOG(DEBUG, "%s.attribute[%d/%d]: %s location:%d size=%d type=0x%x", node_name,
            i + 1, nb_active_attributes, name, info->id, info->size, info->type);

        int ret = ngli_hmap_set(amap, name, info);
        if (ret < 0) {
            ngli_hmap_freep(&amap);
            return NULL;
        }
    }

    return amap;
}

struct hmap *ngli_program_probe_buffer_blocks(const char *node_name, struct glcontext *gl, GLuint pid)
{
    struct hmap *bmap = ngli_hmap_create();
    if (!bmap)
        return NULL;
    ngli_hmap_set_free(bmap, free_pinfo, NULL);

    if (!((gl->features & NGLI_FEATURE_PROGRAM_INTERFACE_QUERY) &&
          (gl->features & NGLI_FEATURE_SHADER_STORAGE_BUFFER_OBJECT)))
        return bmap;

    int nb_active_buffers;
    ngli_glGetProgramInterfaceiv(gl, pid, GL_SHADER_STORAGE_BLOCK,
                                 GL_ACTIVE_RESOURCES, &nb_active_buffers);
    for (int i = 0; i < nb_active_buffers; i++) {
        char name[MAX_ID_LEN] = {0};
        struct bufferprograminfo *info = malloc(sizeof(*info));
        if (!info) {
            ngli_hmap_freep(&bmap);
            return NULL;
        }

        ngli_glGetProgramResourceName(gl, pid, GL_SHADER_STORAGE_BLOCK, i, sizeof(name), NULL, name);
        GLuint block_index = ngli_glGetProgramResourceIndex(gl, pid, GL_SHADER_STORAGE_BLOCK, name);

        static const GLenum props[] = {GL_BUFFER_BINDING};
        ngli_glGetProgramResourceiv(gl, pid, GL_SHADER_STORAGE_BLOCK, block_index,
                                    NGLI_ARRAY_NB(props), props, 1, NULL, &info->binding);

        LOG(DEBUG, "%s.ssbo[%d/%d]: %s binding:%d",
            node_name, i + 1, nb_active_buffers, name, info->binding);

        int ret = ngli_hmap_set(bmap, name, info);
        if (ret < 0) {
            ngli_hmap_freep(&bmap);
            return NULL;
        }
    }

    return bmap;
}

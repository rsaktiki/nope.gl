/*
 * Copyright 2016 GoPro Inc.
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

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hmap.h"
#include "log.h"
#include "nodegl.h"
#include "nodes.h"
#include "params.h"
#include "utils.h"
#include "nodes_register.h"

#define OFFSET(x) offsetof(struct ngl_node, x)
const struct node_param ngli_base_node_params[] = {
    {"glstates", PARAM_TYPE_NODELIST, OFFSET(glstates), .flags=PARAM_FLAG_DOT_DISPLAY_PACKED},
    {"name",     PARAM_TYPE_STR,      OFFSET(name)},
    {NULL}
};

static void *aligned_allocz(size_t size)
{
    void *ptr = NULL;
    if (posix_memalign(&ptr, NGLI_ALIGN, size))
        return NULL;
    memset(ptr, 0, size);
    return ptr;
}

#define ALIGN(v, a) ((v) + (((v) + (a) - 1) & ~((v) - 1)))

static struct ngl_node *node_create(const struct node_class *class)
{
    struct ngl_node *node;
    const size_t node_size = ALIGN(sizeof(*node), NGLI_ALIGN);

    node = aligned_allocz(node_size + class->priv_size);
    if (!node)
        return NULL;
    node->priv_data = ((uint8_t *)node) + node_size;

    /* Make sure the node and its private data are properly aligned */
    ngli_assert((((uintptr_t)node)            & ~(NGLI_ALIGN - 1)) == (uintptr_t)node);
    ngli_assert((((uintptr_t)node->priv_data) & ~(NGLI_ALIGN - 1)) == (uintptr_t)node->priv_data);

    /* We depend on the monotically incrementing by 1 property of these fields */
    ngli_assert(NGL_NODE_UNIFORMVEC4      - NGL_NODE_UNIFORMFLOAT       == 3);
    ngli_assert(NGL_NODE_ANIMKEYFRAMEVEC4 - NGL_NODE_ANIMKEYFRAMEFLOAT  == 3);
    ngli_assert(NGL_NODE_ANIMATEDVEC4     - NGL_NODE_ANIMATEDFLOAT      == 3);

    node->class = class;
    node->last_update_time = -1.;
    node->visit_time = -1.;

    node->refcount = 1;

    node->state = STATE_UNINITIALIZED;

    node->modelview_matrix[ 0] =
    node->modelview_matrix[ 5] =
    node->modelview_matrix[10] =
    node->modelview_matrix[15] = 1.f;

    node->projection_matrix[ 0] =
    node->projection_matrix[ 5] =
    node->projection_matrix[10] =
    node->projection_matrix[15] = 1.f;

    return node;
}

#define DEF_NAME_CHR(c) (((c) >= 'A' && (c) <= 'Z') ? (c) ^ 0x20 : (c))

char *ngli_node_default_name(const char *class_name)
{
    char *name = ngli_strdup(class_name);
    if (!name)
        return NULL;
    for (int i = 0; name[i]; i++)
        name[i] = DEF_NAME_CHR(name[i]);
    return name;
}

int ngli_is_default_name(const char *class_name, const char *str)
{
    const size_t len = strlen(class_name);
    if (len != strlen(str))
        return 0;
    for (int i = 0; i < len; i++)
        if (DEF_NAME_CHR(class_name[i]) != str[i])
            return 0;
    return 1;
}

#define REGISTER_NODE(type_name, class)         \
    case type_name: {                           \
        extern const struct node_class class;   \
        ngli_assert(class.id == type_name);     \
        return &class;                          \
    }                                           \

static const struct node_class *get_node_class(int type)
{
    switch (type) {
        NODE_MAP_TYPE2CLASS(REGISTER_NODE)
    }
    return NULL;
}

struct ngl_node *ngli_node_create_noconstructor(int type)
{
    const struct node_class *class = get_node_class(type);
    if (!class)
        return NULL;

    struct ngl_node *node = node_create(class);
    if (!node)
        return NULL;

    ngli_params_set_defaults((uint8_t *)node, ngli_base_node_params);
    ngli_params_set_defaults(node->priv_data, node->class->params);

    node->name = ngli_node_default_name(node->class->name);
    if (!node->name) {
        ngl_node_unrefp(&node);
        return NULL;
    }

    return node;
}

struct ngl_node *ngl_node_create(int type, ...)
{
    struct ngl_node *node = ngli_node_create_noconstructor(type);
    if (!node)
        return NULL;

    va_list ap;
    va_start(ap, type);
    ngli_params_set_constructors(node->priv_data, node->class->params, &ap);
    va_end(ap);

    LOG(VERBOSE, "CREATED %s @ %p", node->name, node);

    return node;
}

static void node_release(struct ngl_node *node)
{
    if (node->state == STATE_IDLE)
        return;

    if (node->state != STATE_READY)
        return;

    ngli_assert(node->ctx);
    if (node->class->release) {
        LOG(DEBUG, "RELEASE %s @ %p", node->name, node);
        node->class->release(node);
    }
    node->state = STATE_IDLE;
}

static const size_t opt_sizes[] = {
    [PARAM_TYPE_INT]      = sizeof(int),
    [PARAM_TYPE_I64]      = sizeof(int64_t),
    [PARAM_TYPE_DBL]      = sizeof(double),
    [PARAM_TYPE_STR]      = sizeof(char *),
    [PARAM_TYPE_DATA]     = sizeof(void *)             + sizeof(int),
    [PARAM_TYPE_VEC2]     = sizeof(float[2]),
    [PARAM_TYPE_VEC3]     = sizeof(float[3]),
    [PARAM_TYPE_VEC4]     = sizeof(float[4]),
    [PARAM_TYPE_MAT4]     = sizeof(float[4*4]),
    [PARAM_TYPE_NODE]     = sizeof(struct ngl_node *),
    [PARAM_TYPE_NODELIST] = sizeof(struct ngl_node **) + sizeof(int),
    [PARAM_TYPE_DBLLIST]  = sizeof(double *)           + sizeof(int),
    [PARAM_TYPE_NODEDICT] = sizeof(struct hmap *),
};

/*
 * Reset every field of the private data which is not a parameter. This allows
 * the init() to always be called in a clean state.
 */
static void reset_non_params(struct ngl_node *node)
{
    size_t cur_offset = 0;
    const struct node_param *par = node->class->params;
    uint8_t *base_ptr = node->priv_data;

    while (par && par->key) {
        size_t offset = par->offset;
        if (offset != cur_offset)
            memset(base_ptr + cur_offset, 0, offset - cur_offset);
        cur_offset = offset + opt_sizes[par->type];
        par++;
    }
    memset(base_ptr + cur_offset, 0, node->class->priv_size - cur_offset);
}

static void node_uninit(struct ngl_node *node)
{
    if (node->state == STATE_UNINITIALIZED)
        return;

    ngli_assert(node->ctx);
    node_release(node);

    if (node->class->uninit) {
        LOG(VERBOSE, "UNINIT %s @ %p", node->name, node);
        node->class->uninit(node);
    }
    reset_non_params(node);
    node->state = STATE_UNINITIALIZED;
}

static int node_set_children_ctx(uint8_t *base_ptr, const struct node_param *params,
                                 struct ngl_ctx *ctx)
{
    if (!params)
        return 0;
    for (int i = 0; params[i].key; i++) {
        const struct node_param *par = &params[i];

        if (par->type == PARAM_TYPE_NODE) {
            uint8_t *node_p = base_ptr + par->offset;
            struct ngl_node *node = *(struct ngl_node **)node_p;
            if (node) {
                int ret = ngli_node_attach_ctx(node, ctx);
                if (ret < 0)
                    return ret;
            }
        } else if (par->type == PARAM_TYPE_NODELIST) {
            uint8_t *elems_p = base_ptr + par->offset;
            uint8_t *nb_elems_p = base_ptr + par->offset + sizeof(struct ngl_node **);
            struct ngl_node **elems = *(struct ngl_node ***)elems_p;
            const int nb_elems = *(int *)nb_elems_p;
            for (int j = 0; j < nb_elems; j++) {
                int ret = ngli_node_attach_ctx(elems[j], ctx);
                if (ret < 0)
                    return ret;
            }
        } else if (par->type == PARAM_TYPE_NODEDICT) {
            struct hmap *hmap = *(struct hmap **)(base_ptr + par->offset);
            if (!hmap)
                continue;
            const struct hmap_entry *entry = NULL;
            while ((entry = ngli_hmap_next(hmap, entry))) {
                struct ngl_node *node = entry->data;
                int ret = ngli_node_attach_ctx(node, ctx);
                if (ret < 0)
                    return ret;
            }
        }
    }
    return 0;
}

static int node_set_ctx(struct ngl_node *node, struct ngl_ctx *ctx)
{
    int ret;

    if (ctx) {
        if (node->ctx) {
            if (node->ctx != ctx) {
                LOG(ERROR, "\"%s\" is associated with another rendering context", node->name);
                return -1;
            }
        } else {
            node->ctx = ctx;
        }
    } else {
        node_uninit(node);
        node->ctx = NULL;
    }

    if ((ret = node_set_children_ctx(node->priv_data, node->class->params, ctx)) < 0 ||
        (ret = node_set_children_ctx((uint8_t *)node, ngli_base_node_params, ctx)) < 0)
        return ret;
    return 0;
}

int ngli_node_attach_ctx(struct ngl_node *node, struct ngl_ctx *ctx)
{
    return node_set_ctx(node, ctx);
}

void ngli_node_detach_ctx(struct ngl_node *node)
{
    int ret = node_set_ctx(node, NULL);
    ngli_assert(ret == 0);
}

int ngli_node_init(struct ngl_node *node)
{
    if (node->state == STATE_INITIALIZED)
        return 0;

    if (node->state != STATE_UNINITIALIZED)
        return 0;

    ngli_assert(node->ctx);
    if (node->class->init) {
        LOG(VERBOSE, "INIT %s @ %p", node->name, node);
        int ret = node->class->init(node);
        if (ret < 0)
            return ret;
    }

    for (int i = 0; i < node->nb_glstates; i++) {
        int ret = ngli_node_init(node->glstates[i]);
        if (ret < 0)
            return ret;
    }

    node->state = STATE_INITIALIZED;

    return 0;
}

int ngli_node_visit(struct ngl_node *node, const struct ngl_node *from, double t)
{
    int ret = ngli_node_init(node);
    if (ret < 0)
        return ret;

    if (node->class->visit)
        return node->class->visit(node, from, t);

    node->is_active = from ? from->is_active : 1;
    node->visit_time = t;

    uint8_t *base_ptr = node->priv_data;
    const struct node_param *par = node->class->params;

    while (par && par->key) {
        switch (par->type) {
            case PARAM_TYPE_NODE: {
                uint8_t *child_p = base_ptr + par->offset;
                struct ngl_node *child = *(struct ngl_node **)child_p;
                if (child) {
                    ret = ngli_node_visit(child, node, t);
                    if (ret < 0)
                        return ret;
                }
                break;
            }
            case PARAM_TYPE_NODELIST: {
                uint8_t *elems_p = base_ptr + par->offset;
                uint8_t *nb_elems_p = base_ptr + par->offset + sizeof(struct ngl_node **);
                struct ngl_node **elems = *(struct ngl_node ***)elems_p;
                const int nb_elems = *(int *)nb_elems_p;
                for (int i = 0; i < nb_elems; i++) {
                    ret = ngli_node_visit(elems[i], node, t);
                    if (ret < 0)
                        return ret;
                }
                break;
            }
            case PARAM_TYPE_NODEDICT: {
                struct hmap *hmap = *(struct hmap **)(base_ptr + par->offset);
                if (!hmap)
                    break;
                const struct hmap_entry *entry = NULL;
                while ((entry = ngli_hmap_next(hmap, entry))) {
                    ret = ngli_node_visit(entry->data, node, t);
                    if (ret < 0)
                        return ret;
                }
                break;
            }
        }
        par++;
    }

    return 0;
}

static int node_prefetch(struct ngl_node *node)
{
    if (node->state == STATE_READY)
        return 0;

    int ret = ngli_node_init(node);
    if (ret < 0)
        return ret;

    if (node->class->prefetch) {
        LOG(DEBUG, "PREFETCH %s @ %p", node->name, node);
        ret = node->class->prefetch(node);
        if (ret < 0)
            return ret;
    }
    node->state = STATE_READY;

    return 0;
}

int ngli_node_honor_release_prefetch(struct ngl_node *node, double t)
{
    uint8_t *base_ptr = node->priv_data;
    const struct node_param *par = node->class->params;

    if (node->visit_time != t)
        return 0;

    while (par && par->key) {
        switch (par->type) {
            case PARAM_TYPE_NODE: {
                uint8_t *child_p = base_ptr + par->offset;
                struct ngl_node *child = *(struct ngl_node **)child_p;
                if (child) {
                    int ret = ngli_node_honor_release_prefetch(child, t);
                    if (ret < 0)
                        return ret;
                }
                break;
            }
            case PARAM_TYPE_NODELIST: {
                uint8_t *elems_p = base_ptr + par->offset;
                uint8_t *nb_elems_p = base_ptr + par->offset + sizeof(struct ngl_node **);
                struct ngl_node **elems = *(struct ngl_node ***)elems_p;
                const int nb_elems = *(int *)nb_elems_p;
                for (int i = 0; i < nb_elems; i++) {
                    int ret = ngli_node_honor_release_prefetch(elems[i], t);
                    if (ret < 0)
                        return ret;
                }
                break;
            }
            case PARAM_TYPE_NODEDICT: {
                struct hmap *hmap = *(struct hmap **)(base_ptr + par->offset);
                if (!hmap)
                    break;
                const struct hmap_entry *entry = NULL;
                while ((entry = ngli_hmap_next(hmap, entry))) {
                    int ret = ngli_node_honor_release_prefetch(entry->data, t);
                    if (ret < 0)
                        return ret;
                }
                break;
            }
        }
        par++;
    }

    if (node->is_active)
        return node_prefetch(node);

    node_release(node);
    return 0;
}

int ngli_node_update(struct ngl_node *node, double t)
{
    int ret = ngli_node_init(node);
    if (ret < 0)
        return ret;
    if (node->class->update) {
        if (node->last_update_time != t) {
            // Sometimes the node might not be prefetched by the node_check_prefetch()
            // crawling: this could happen when the node was for instance instantiated
            // internally and not through the options. So just to be safe, we
            // "prefetch" it now (a bit late for sure).
            ret = node_prefetch(node);
            if (ret < 0)
                return ret;

            LOG(VERBOSE, "UPDATE %s @ %p with t=%g", node->name, node, t);
            ret = node->class->update(node, t);
            if (ret < 0)
                return ret;
        } else {
            LOG(VERBOSE, "%s already updated for t=%g, skip it", node->name, t);
        }
        node->last_update_time = t;
    }

    return 0;
}

void ngli_honor_glstates(struct ngl_ctx *ctx, int nb_glstates, struct ngl_node **glstates)
{
    struct glcontext *glcontext = ctx->glcontext;
    const struct glfunctions *gl = &glcontext->funcs;

    for (int i = 0; i < nb_glstates; i++) {
        struct ngl_node *stnode = glstates[i];
        struct glstate *st = stnode->priv_data;

        if (stnode->class->id == NGL_NODE_GLBLENDSTATE) {
            ngli_glGetIntegerv(gl, st->capability, (GLint *)&st->enabled[1]);
            if (st->enabled[0]) {
                ngli_glGetIntegerv(gl, GL_BLEND_SRC_RGB, (GLint *)&st->src_rgb[1]);
                ngli_glGetIntegerv(gl, GL_BLEND_DST_RGB, (GLint *)&st->dst_rgb[1]);
                ngli_glGetIntegerv(gl, GL_BLEND_SRC_ALPHA, (GLint *)&st->src_alpha[1]);
                ngli_glGetIntegerv(gl, GL_BLEND_DST_ALPHA, (GLint *)&st->dst_alpha[1]);
                ngli_glGetIntegerv(gl, GL_BLEND_EQUATION_RGB, (GLint *)&st->mode_rgb[1]);
                ngli_glGetIntegerv(gl, GL_BLEND_EQUATION_ALPHA, (GLint *)&st->mode_alpha[1]);
                ngli_glEnable(gl, st->capability);
                ngli_glBlendFuncSeparate(gl, st->src_rgb[0], st->dst_rgb[0], st->src_alpha[0], st->dst_alpha[0]);
                ngli_glBlendEquationSeparate(gl, st->mode_rgb[0], st->mode_alpha[0]);
            } else {
                ngli_glDisable(gl, st->capability);
            }
        } else if (stnode->class->id == NGL_NODE_GLCOLORSTATE) {
            GLboolean rgba[4];
            ngli_glGetBooleanv(gl, GL_COLOR_WRITEMASK, rgba);
            st->rgba[1][0] = rgba[0];
            st->rgba[1][1] = rgba[1];
            st->rgba[1][2] = rgba[2];
            st->rgba[1][3] = rgba[3];
            ngli_glColorMask(gl, st->rgba[0][0], st->rgba[0][1], st->rgba[0][2], st->rgba[0][3]);
        } else if (stnode->class->id == NGL_NODE_GLPOLYGONMODESTATE) {
            ngli_glGetIntegerv(gl, GL_POLYGON_MODE, (GLint *)&st->mode[1]);
            ngli_glPolygonMode(gl, GL_FRONT_AND_BACK, st->mode[0]);
        } else if (stnode->class->id == NGL_NODE_GLSTENCILSTATE) {
            ngli_glGetIntegerv(gl, st->capability, (GLint *)&st->enabled[1]);
            if (st->enabled[0]) {
                ngli_glGetIntegerv(gl, GL_STENCIL_WRITEMASK, (GLint *)&st->writemask[1]);
                ngli_glGetIntegerv(gl, GL_STENCIL_FUNC, (GLint *)&st->func[1]);
                ngli_glGetIntegerv(gl, GL_STENCIL_REF, (GLint *)&st->func_ref[1]);
                ngli_glGetIntegerv(gl, GL_STENCIL_VALUE_MASK, (GLint *)&st->func_mask[1]);
                ngli_glGetIntegerv(gl, GL_STENCIL_FAIL, (GLint *)&st->op_sfail[1]);
                ngli_glGetIntegerv(gl, GL_STENCIL_PASS_DEPTH_FAIL, (GLint *)&st->op_dpfail[1]);
                ngli_glGetIntegerv(gl, GL_STENCIL_PASS_DEPTH_PASS, (GLint *)&st->op_dppass[1]);
                ngli_glEnable(gl, st->capability);
                ngli_glStencilMask(gl, st->writemask[0]);
                ngli_glStencilFunc(gl, st->func[0], st->func_ref[0], st->func_mask[0]);
                ngli_glStencilOp(gl, st->op_sfail[0], st->op_dpfail[0], st->op_dppass[0]);
            } else {
                ngli_glDisable(gl, st->capability);
            }
        } else {
            ngli_glGetIntegerv(gl, st->capability, (GLint *)&st->enabled[1]);
            if (st->enabled[0] != st->enabled[1]) {
                if (st->enabled[0]) {
                    ngli_glEnable(gl, st->capability);
                } else {
                    ngli_glDisable(gl, st->capability);
                }
            }
        }
    }
}

void ngli_restore_glstates(struct ngl_ctx *ctx, int nb_glstates, struct ngl_node **glstates)
{
    struct glcontext *glcontext = ctx->glcontext;
    const struct glfunctions *gl = &glcontext->funcs;

    for (int i = 0; i < nb_glstates; i++) {
        struct ngl_node *stnode = glstates[i];
        struct glstate *st = glstates[i]->priv_data;
        if (stnode->class->id == NGL_NODE_GLBLENDSTATE) {
            if (st->enabled[1]) {
                ngli_glEnable(gl, st->capability);
                ngli_glBlendFuncSeparate(gl, st->src_rgb[1], st->dst_rgb[1], st->src_alpha[1], st->dst_alpha[1]);
                ngli_glBlendEquationSeparate(gl, st->mode_rgb[1], st->mode_alpha[1]);
            } else {
                ngli_glDisable(gl, st->capability);
            }
        } else if (stnode->class->id == NGL_NODE_GLCOLORSTATE) {
            ngli_glColorMask(gl, st->rgba[1][0], st->rgba[1][1], st->rgba[1][2], st->rgba[1][3]);
        } else if (stnode->class->id == NGL_NODE_GLPOLYGONMODESTATE) {
            ngli_glPolygonMode(gl, GL_FRONT_AND_BACK, st->mode[1]);
        } else if (stnode->class->id == NGL_NODE_GLSTENCILSTATE) {
            if (st->enabled[1]) {
                ngli_glEnable(gl, st->capability);
                ngli_glStencilMask(gl, st->writemask[1]);
                ngli_glStencilFunc(gl, st->func[1], st->func_ref[1], st->func_mask[1]);
                ngli_glStencilOp(gl, st->op_sfail[1], st->op_dpfail[1], st->op_dppass[1]);
            } else {
                ngli_glDisable(gl, st->capability);
            }
        } else {
            if (st->enabled[0] != st->enabled[1]) {
                if (st->enabled[1]) {
                    ngli_glEnable(gl, st->capability);
                } else {
                    ngli_glDisable(gl, st->capability);
                }
            }
        }
    }
}

void ngli_node_draw(struct ngl_node *node)
{
    if (node->class->draw) {
        LOG(VERBOSE, "DRAW %s @ %p", node->name, node);

        ngli_honor_glstates(node->ctx, node->nb_glstates, node->glstates);
        node->class->draw(node);
        ngli_restore_glstates(node->ctx, node->nb_glstates, node->glstates);
    }
}

const struct node_param *ngli_node_param_find(const struct ngl_node *node, const char *key,
                                              uint8_t **base_ptrp)
{
    const struct node_param *par = ngli_params_find(ngli_base_node_params, key);
    *base_ptrp = (uint8_t *)node;

    if (!par) {
        par = ngli_params_find(node->class->params, key);
        *base_ptrp = (uint8_t *)node->priv_data;
    }
    if (!par)
        LOG(ERROR, "parameter %s not found in %s", key, node->class->name);
    return par;
}

int ngl_node_param_add(struct ngl_node *node, const char *key,
                       int nb_elems, void *elems)
{
    int ret = 0;

    uint8_t *base_ptr;
    const struct node_param *par = ngli_node_param_find(node, key, &base_ptr);
    if (!par)
        return -1;

    ret = ngli_params_add(base_ptr, par, nb_elems, elems);
    if (ret < 0)
        LOG(ERROR, "unable to add elements to %s.%s", node->name, key);
    node_uninit(node); // need a reinit after changing options
    return ret;
}

int ngl_node_param_set(struct ngl_node *node, const char *key, ...)
{
    int ret = 0;
    va_list ap;

    uint8_t *base_ptr;
    const struct node_param *par = ngli_node_param_find(node, key, &base_ptr);
    if (!par)
        return -1;

    va_start(ap, key);
    ret = ngli_params_set(base_ptr, par, &ap);
    if (ret < 0)
        LOG(ERROR, "unable to set %s.%s", node->name, key);
    va_end(ap);
    node_uninit(node); // need a reinit after changing options
    return ret;
}

struct ngl_node *ngl_node_ref(struct ngl_node *node)
{
    node->refcount++;
    return node;
}

void ngl_node_unrefp(struct ngl_node **nodep)
{
    int delete = 0;
    struct ngl_node *node = *nodep;

    if (!node)
        return;
    delete = node->refcount-- == 1;
    if (delete) {
        LOG(VERBOSE, "DELETE %s @ %p", node->name, node);
        ngli_assert(!node->ctx);
        ngli_params_free((uint8_t *)node, ngli_base_node_params);
        ngli_params_free(node->priv_data, node->class->params);
        free(node);
    }
    *nodep = NULL;
}

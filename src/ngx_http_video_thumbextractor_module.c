/*
 * Copyright (C) 2011 Wandenberg Peixoto <wandenberg@gmail.com>
 *
 * This file is part of Nginx Video Thumb Extractor Module.
 *
 * Nginx Video Thumb Extractor Module is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Nginx Video Thumb Extractor Module is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Nginx Video Thumb Extractor Module.  If not, see <http://www.gnu.org/licenses/>.
 *
 *
 * ngx_http_video_thumbextractor_module.c
 *
 * Created:  Nov 22, 2011
 * Author:   Wandenberg Peixoto <wandenberg@gmail.com>
 *
 */
#include <ngx_http_video_thumbextractor_module.h>
#include <ngx_http_video_thumbextractor_module_setup.c>
#include <ngx_http_video_thumbextractor_module_utils.c>
#include <ngx_http_video_thumbextractor_module_ipc.c>

ngx_http_output_header_filter_pt ngx_http_video_thumbextractor_next_header_filter;
ngx_http_output_body_filter_pt ngx_http_video_thumbextractor_next_body_filter;

ngx_int_t ngx_http_video_thumbextractor_extract_and_send_thumb(ngx_http_request_t *r);
ngx_int_t ngx_http_video_thumbextractor_set_request_context(ngx_http_request_t *r);
void      ngx_http_video_thumbextractor_cleanup_request_context(ngx_http_request_t *r);


static ngx_int_t
ngx_http_video_thumbextractor_header_filter(ngx_http_request_t *r)
{
    ngx_http_video_thumbextractor_loc_conf_t *vtlcf;
    ngx_http_video_thumbextractor_ctx_t      *ctx;

    vtlcf = ngx_http_get_module_loc_conf(r, ngx_http_video_thumbextractor_module);

    if (!vtlcf->enabled) {
        return ngx_http_video_thumbextractor_next_header_filter(r);
    }

    ctx = ngx_http_get_module_ctx(r, ngx_http_video_thumbextractor_module);

    if (ctx != NULL) {
        return ngx_http_video_thumbextractor_next_header_filter(r);
    }

    return NGX_OK;
}


static ngx_int_t
ngx_http_video_thumbextractor_body_filter(ngx_http_request_t *r, ngx_chain_t *in)
{
    ngx_http_video_thumbextractor_loc_conf_t *vtlcf;
    ngx_http_video_thumbextractor_ctx_t      *ctx;
    ngx_chain_t                              *cl;
    ngx_flag_t                                last_buf = 0;
    ngx_int_t                                 rc;

    vtlcf = ngx_http_get_module_loc_conf(r, ngx_http_video_thumbextractor_module);

    if (!vtlcf->enabled) {
        return ngx_http_video_thumbextractor_next_body_filter(r, in);
    }

    ctx = ngx_http_get_module_ctx(r, ngx_http_video_thumbextractor_module);

    if ((ctx != NULL) && (r->headers_out.status >= NGX_HTTP_BAD_REQUEST)) {
        return ngx_http_video_thumbextractor_next_body_filter(r, in);
    }

    if (in == NULL) {
        return ngx_http_video_thumbextractor_next_body_filter(r, in);
    }

    // discard chains from original content
    for (cl = in; cl; cl = cl->next) {
        cl->buf->pos = cl->buf->last;
        last_buf = (cl->buf->last_buf) ? 1 : last_buf;
    }

    if (!last_buf) {
        return NGX_OK;
    }

    // clear values from original content
    ngx_http_clear_content_length(r);
    ngx_http_clear_accept_ranges(r);
    ngx_http_clear_last_modified(r);

    if ((rc = ngx_http_video_thumbextractor_set_request_context(r)) != NGX_OK) {
        return ngx_http_filter_finalize_request(r, &ngx_http_video_thumbextractor_module, (rc == NGX_ERROR) ? NGX_HTTP_INTERNAL_SERVER_ERROR : rc);
    }

    return ngx_http_video_thumbextractor_extract_and_send_thumb(r);
}


ngx_int_t
ngx_http_video_thumbextractor_extract_and_send_thumb(ngx_http_request_t *r)
{
    ngx_http_video_thumbextractor_main_conf_t *vtmcf;
    ngx_http_video_thumbextractor_ctx_t       *ctx;
    ngx_http_video_thumbextractor_ipc_t       *ipc_ctx;

    if ((ipc_ctx = ngx_pcalloc(r->pool, sizeof(ngx_http_video_thumbextractor_ipc_t))) == NULL) {
        ngx_log_error(NGX_LOG_CRIT, r->connection->log, 0, "video thumb extractor module: unable to allocate memory to keep reference to request context before fork the process");
        return ngx_http_filter_finalize_request(r, &ngx_http_video_thumbextractor_module, NGX_HTTP_INTERNAL_SERVER_ERROR);
    }

    ctx = ngx_http_get_module_ctx(r, ngx_http_video_thumbextractor_module);

    ngx_queue_init(&ipc_ctx->queue);
    ipc_ctx->info.filename = ctx->filename;
    ipc_ctx->info.offset = 0;

#if (NGX_HTTP_CACHE)
    if (r->cache) {
        if (r->headers_out.status >= NGX_HTTP_BAD_REQUEST) {
            ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, "video thumb extractor module: cached file isn't a success result to extract an image");
            return ngx_http_filter_finalize_request(r, &ngx_http_video_thumbextractor_module, NGX_HTTP_NOT_FOUND);
        }

        if ((ipc_ctx->info.filename = ngx_http_video_thumbextractor_create_str(r->pool, r->cache->file.name.len)) == NULL) {
            ngx_log_error(NGX_LOG_CRIT, r->connection->log, 0, "video thumb extractor module: unable to allocate memory to copy proxy cache full filename");
            return ngx_http_filter_finalize_request(r, &ngx_http_video_thumbextractor_module, NGX_HTTP_INTERNAL_SERVER_ERROR);
        }

        ngx_memcpy(ipc_ctx->info.filename->data, r->cache->file.name.data, r->cache->file.name.len);
        ipc_ctx->info.offset = r->cache->body_start;
    }
#endif

    ctx->ipc_ctx = ipc_ctx;
    ipc_ctx->request = r;

    r->main->count++;

    ngx_queue_insert_tail(ngx_http_video_thumbextractor_module_extract_queue, &ipc_ctx->queue);

    vtmcf = ngx_http_get_module_main_conf(r, ngx_http_video_thumbextractor_module);
    ngx_http_video_thumbextractor_module_ensure_extractor_process(vtmcf);

    return NGX_DONE;
}


ngx_int_t
ngx_http_video_thumbextractor_set_request_context(ngx_http_request_t *r)
{
    ngx_http_video_thumbextractor_loc_conf_t    *vtlcf;
    ngx_http_video_thumbextractor_ctx_t         *ctx;
    ngx_pool_cleanup_t                          *cln;
    ngx_http_core_loc_conf_t                    *clcf;
    ngx_str_t                                    vv_filename = ngx_null_string, vv_second = ngx_null_string;
    ngx_str_t                                    vv_width = ngx_null_string, vv_height = ngx_null_string;

    vtlcf = ngx_http_get_module_loc_conf(r, ngx_http_video_thumbextractor_module);
    clcf = ngx_http_get_module_loc_conf(r, ngx_http_core_module);

    ctx = ngx_http_get_module_ctx(r, ngx_http_video_thumbextractor_module);

    if (ctx != NULL) {
        return NGX_OK;
    }

    if ((cln = ngx_pool_cleanup_add(r->pool, 0)) == NULL) {
        ngx_log_error(NGX_LOG_CRIT, r->connection->log, 0, "video thumb extractor module: unable to allocate memory for cleanup");
        return NGX_ERROR;
    }

    // set a cleaner to request
    cln->handler = (ngx_pool_cleanup_pt) ngx_http_video_thumbextractor_cleanup_request_context;
    cln->data = r;

    if ((ctx = ngx_pcalloc(r->pool, sizeof(ngx_http_video_thumbextractor_ctx_t))) == NULL) {
        return NGX_ERROR;
    }

    ngx_http_set_ctx(r, ctx, ngx_http_video_thumbextractor_module);

    // check if received a filename
    ngx_http_complex_value(r, vtlcf->video_filename, &vv_filename);
    NGX_HTTP_VIDEO_THUMBEXTRACTOR_VARIABLE_REQUIRED(vv_filename, r->connection->log, "filename variable is empty");

    ngx_http_complex_value(r, vtlcf->video_second, &vv_second);
    NGX_HTTP_VIDEO_THUMBEXTRACTOR_VARIABLE_REQUIRED(vv_second, r->connection->log, "second variable is empty");

    ctx->second = ngx_atoi(vv_second.data, vv_second.len);
    if (ctx->second == NGX_ERROR) {
        ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, "video thumb extractor module: Invalid second %V", &vv_second);
        return NGX_HTTP_BAD_REQUEST;
    }

    if (vtlcf->image_width != NULL) {
        ngx_http_complex_value(r, vtlcf->image_width, &vv_width);
        ctx->width = ngx_atoi(vv_width.data, vv_width.len);
        ctx->width = (ctx->width != NGX_ERROR) ? ctx->width : 0;
    }

    if (vtlcf->image_height != NULL) {
        ngx_http_complex_value(r, vtlcf->image_height, &vv_height);
        ctx->height = ngx_atoi(vv_height.data, vv_height.len);
        ctx->height = (ctx->height != NGX_ERROR) ? ctx->height : 0;
    }

    if (((ctx->width > 0) && (ctx->width < 16)) || ((ctx->height > 0) && (ctx->height < 16))) {
        ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, "video thumb extractor module: Very small size requested, %d x %d", ctx->width, ctx->height);
        return NGX_HTTP_BAD_REQUEST;
    }

    if ((ctx->filename = ngx_http_video_thumbextractor_create_str(r->pool, clcf->root.len + vv_filename.len)) == NULL) {
        ngx_log_error(NGX_LOG_CRIT, r->connection->log, 0, "video thumb extractor module: unable to allocate memory to store full filename");
        return NGX_ERROR;
    }
    ngx_memcpy(ngx_copy(ctx->filename->data, clcf->root.data, clcf->root.len), vv_filename.data, vv_filename.len);

    return NGX_OK;
}


ngx_int_t
ngx_http_video_thumbextractor_filter_init(ngx_conf_t *cf)
{
    ngx_http_video_thumbextractor_next_header_filter = ngx_http_top_header_filter;
    ngx_http_top_header_filter = ngx_http_video_thumbextractor_header_filter;

    ngx_http_video_thumbextractor_next_body_filter = ngx_http_top_body_filter;
    ngx_http_top_body_filter = ngx_http_video_thumbextractor_body_filter;

    return NGX_OK;
}


void
ngx_http_video_thumbextractor_cleanup_request_context(ngx_http_request_t *r)
{
    ngx_http_video_thumbextractor_main_conf_t *vtmcf = ngx_http_get_module_main_conf(r, ngx_http_video_thumbextractor_module);
    ngx_http_video_thumbextractor_ctx_t       *ctx = ngx_http_get_module_ctx(r, ngx_http_video_thumbextractor_module);

    r->read_event_handler = ngx_http_request_empty_handler;

    if (ctx != NULL) {
        if (ctx->ipc_ctx != NULL) {
            ctx->ipc_ctx->request = NULL;

            if (!ngx_queue_empty(&ctx->ipc_ctx->queue)) {
                ngx_queue_remove(&ctx->ipc_ctx->queue);
            }
        }
    }

    ngx_http_video_thumbextractor_module_ensure_extractor_process(vtmcf);
}

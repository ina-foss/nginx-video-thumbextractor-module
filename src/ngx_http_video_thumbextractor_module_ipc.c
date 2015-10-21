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
 * ngx_http_video_thumbextractor_module_utils.c
 *
 * Created:  Nov 22, 2011
 * Author:   Wandenberg Peixoto <wandenberg@gmail.com>
 *
 */

ngx_http_output_header_filter_pt ngx_http_video_thumbextractor_next_header_filter;
ngx_http_output_body_filter_pt ngx_http_video_thumbextractor_next_body_filter;

void        ngx_http_video_thumbextractor_fork_extract_process(ngx_uint_t slot);
void        ngx_http_video_thumbextractor_run_extract(ngx_http_video_thumbextractor_ipc_t *ipc_ctx);
void        ngx_http_video_thumbextractor_extract_process_read_handler(ngx_event_t *ev);
void        ngx_http_video_thumbextractor_extract_process_write_handler(ngx_event_t *ev);
ngx_int_t   ngx_http_video_thumbextractor_recv(ngx_connection_t *c, ngx_event_t *rev, ngx_buf_t *buf, ssize_t len);
ngx_int_t   ngx_http_video_thumbextractor_write(ngx_connection_t *c, ngx_event_t *wev, ngx_buf_t *buf, ssize_t len);
void        ngx_http_video_thumbextractor_set_buffer(ngx_buf_t *buf, u_char *start, u_char *last, ssize_t len);


void
ngx_http_video_thumbextractor_module_ensure_extractor_process(ngx_http_video_thumbextractor_main_conf_t *vtmcf)
{
    ngx_int_t                                    slot = -1;
    ngx_uint_t                                   i;

    if (ngx_queue_empty(ngx_http_video_thumbextractor_module_extract_queue)) {
        return;
    }

    for (i = 0; i < vtmcf->processes_per_worker; ++i) {
        if (ngx_http_video_thumbextractor_module_child_pids[i] == -1) {
            slot = i;
            break;
        }
    }

    if (slot >= 0) {
        ngx_http_video_thumbextractor_fork_extract_process(slot);
    }
}


void
ngx_http_video_thumbextractor_fork_extract_process(ngx_uint_t slot)
{
    ngx_http_video_thumbextractor_ipc_t      *ipc_ctx;
    ngx_queue_t                              *q;
    int                                       ret;
    ngx_pid_t                                 pid;
    ngx_event_t                              *rev;

    q = ngx_queue_head(ngx_http_video_thumbextractor_module_extract_queue);
    ngx_queue_remove(q);
    ngx_queue_init(q);
    ipc_ctx = ngx_queue_data(q, ngx_http_video_thumbextractor_ipc_t, queue);

    ipc_ctx->pipefd[0] = -1;
    ipc_ctx->pipefd[1] = -1;
    ipc_ctx->slot = slot;

    if (pipe(ipc_ctx->pipefd) == -1) {
        ngx_log_error(NGX_LOG_ERR, ngx_cycle->log, ngx_errno, "video thumb extractor module: unable to initialize a pipe");
        return;
    }

    /* make pipe write end survive through exec */

    ret = fcntl(ipc_ctx->pipefd[1], F_GETFD);

    if (ret != -1) {
        ret &= ~FD_CLOEXEC;
        ret = fcntl(ipc_ctx->pipefd[1], F_SETFD, ret);
    }

    if (ret == -1) {
        close(ipc_ctx->pipefd[0]);
        close(ipc_ctx->pipefd[1]);

        ngx_log_error(NGX_LOG_ERR, ngx_cycle->log, ngx_errno, "video thumb extractor module: unable to make pipe write end live longer");
        return;
    }

    /* ignore the signal when the child dies */
    signal(SIGCHLD, SIG_IGN);

    pid = fork();

    switch (pid) {

    case -1:
        /* failure */
        if (ipc_ctx->pipefd[0] != -1) {
            close(ipc_ctx->pipefd[0]);
        }

        if (ipc_ctx->pipefd[1] != -1) {
            close(ipc_ctx->pipefd[1]);
        }

        ngx_log_error(NGX_LOG_ERR, ngx_cycle->log, ngx_errno, "video thumb extractor module: unable to fork the process");

        break;

    case 0:
        /* child */

#if (NGX_LINUX)
        prctl(PR_SET_PDEATHSIG, SIGKILL, 0, 0, 0);
#endif

        ngx_setproctitle("thumb extractor");
        ngx_http_video_thumbextractor_run_extract(ipc_ctx);
        break;

    default:
        /* parent */
        if (ipc_ctx->pipefd[1] != -1) {
            close(ipc_ctx->pipefd[1]);
        }

        if (ipc_ctx->pipefd[0] != -1) {
            ngx_http_video_thumbextractor_module_child_pids[slot] = pid;
            ipc_ctx->conn = ngx_get_connection(ipc_ctx->pipefd[0], ngx_cycle->log);
            ipc_ctx->conn->data = ipc_ctx;

            rev = ipc_ctx->conn->read;
            rev->handler = ngx_http_video_thumbextractor_extract_process_read_handler;

            if (ngx_add_event(rev, NGX_READ_EVENT, 0) != NGX_OK) {
                ngx_log_error(NGX_LOG_ERR, ngx_cycle->log, ngx_errno, "video thumb extractor module: failed to add child control event");
            }
        }
        break;
    }
}


void
ngx_http_video_thumbextractor_run_extract(ngx_http_video_thumbextractor_ipc_t *ipc_ctx)
{
    ngx_http_video_thumbextractor_loc_conf_t  *vtlcf;
    ngx_http_video_thumbextractor_ctx_t       *ctx;
    ngx_http_video_thumbextractor_transfer_t  *transfer;
    ngx_http_request_t                        *r;
    ngx_pool_t                                *temp_pool;
    ngx_event_t                               *wev;
    ngx_int_t                                  rc = NGX_ERROR;

    if ((temp_pool = ngx_create_pool(4096, ngx_cycle->log)) == NULL) {
        ngx_log_error(NGX_LOG_CRIT, ngx_cycle->log, 0, "video thumb extractor module: unable to allocate temporary pool to extract the thumb");
        if (ngx_write_fd(ipc_ctx->pipefd[1], &rc, sizeof(ngx_int_t)) <= 0) {
            exit(1);
        }
    }

    if ((transfer = ngx_pcalloc(temp_pool, sizeof(ngx_http_video_thumbextractor_transfer_t))) == NULL) {
        ngx_log_error(NGX_LOG_CRIT, ngx_cycle->log, 0, "video thumb extractor module: unable to allocate transfer structure");
        if (ngx_write_fd(ipc_ctx->pipefd[1], &rc, sizeof(ngx_int_t)) <= 0) {
            exit(1);
        }
    }

    r = ipc_ctx->request;
    if (r == NULL) {
        if (ngx_write_fd(ipc_ctx->pipefd[1], &rc, sizeof(ngx_int_t)) <= 0) {
            exit(1);
        }
    }

    vtlcf = ngx_http_get_module_loc_conf(r, ngx_http_video_thumbextractor_module);
    ctx = ngx_http_get_module_ctx(r, ngx_http_video_thumbextractor_module);

    transfer->data = 0;
    transfer->size = 0;

    transfer->rc = ngx_http_video_thumbextractor_get_thumb(vtlcf, ctx, &ipc_ctx->info, &transfer->data, &transfer->size, temp_pool, r->connection->log);

    transfer->rc_transfered = 0;
    transfer->size_transfered = 0;
    ngx_http_video_thumbextractor_set_buffer(&transfer->buffer, (u_char *) &transfer->rc, NULL, sizeof(ngx_int_t));

    transfer->conn = ngx_get_connection(ipc_ctx->pipefd[1], ngx_cycle->log);
    transfer->conn->data = transfer;

    wev = transfer->conn->write;
    wev->handler = ngx_http_video_thumbextractor_extract_process_write_handler;

    if (ngx_add_event(wev, NGX_WRITE_EVENT, 0) != NGX_OK) {
        ngx_log_error(NGX_LOG_ERR, ngx_cycle->log, ngx_errno, "video thumb extractor module: failed to add child write event");
    }
}


void
ngx_http_video_thumbextractor_extract_process_read_handler(ngx_event_t *ev)
{
    ngx_http_video_thumbextractor_main_conf_t *vtmcf;
    ngx_http_video_thumbextractor_ipc_t       *ipc_ctx;
    ngx_http_video_thumbextractor_transfer_t  *transfer;
    ngx_connection_t                          *c;
    ngx_http_request_t                        *r;
    ngx_chain_t                               *out;
    ngx_int_t                                  rc;
    ngx_queue_t                               *q;

    c = ev->data;
    ipc_ctx = c->data;
    transfer = ipc_ctx->transfer;
    r = ipc_ctx->request;

    if (r == NULL) {
        ngx_log_debug(NGX_LOG_DEBUG, ngx_cycle->log, 0, "video thumb extractor module: request already gone");
        goto exit;
    }

    if (transfer == NULL) {
        if ((transfer = ngx_pcalloc(r->pool, sizeof(ngx_http_video_thumbextractor_transfer_t))) == NULL) {
            ngx_log_error(NGX_LOG_CRIT, r->connection->log, 0, "video thumb extractor module: unable to allocate transfer structure");
            ngx_http_filter_finalize_request(r, &ngx_http_video_thumbextractor_module, NGX_HTTP_INTERNAL_SERVER_ERROR);
            goto finalize;
        }

        transfer->rc_transfered = 0;
        transfer->size_transfered = 0;
        ipc_ctx->transfer = transfer;

        ngx_http_video_thumbextractor_set_buffer(&transfer->buffer, (u_char *) &transfer->rc, NULL, sizeof(ngx_int_t));
    }

    ngx_http_video_thumbextractor_set_buffer(&transfer->buffer, transfer->buffer.start, transfer->buffer.last, 0);

    if (!transfer->rc_transfered) {
        if ((rc = ngx_http_video_thumbextractor_recv(c, ev, &transfer->buffer, sizeof(ngx_int_t))) != NGX_OK) {
            goto transfer_failed;
        }

        ngx_http_video_thumbextractor_set_buffer(&transfer->buffer, (u_char *) &transfer->size, NULL, sizeof(size_t));
        transfer->rc_transfered = 1;
    }

    if (transfer->rc == NGX_ERROR) {
        ngx_http_filter_finalize_request(r, &ngx_http_video_thumbextractor_module, NGX_HTTP_INTERNAL_SERVER_ERROR);
        goto finalize;
    }

    if ((transfer->rc == NGX_HTTP_VIDEO_THUMBEXTRACTOR_FILE_NOT_FOUND) || (transfer->rc == NGX_HTTP_VIDEO_THUMBEXTRACTOR_SECOND_NOT_FOUND)) {
        ngx_http_filter_finalize_request(r, &ngx_http_video_thumbextractor_module, NGX_HTTP_NOT_FOUND);
        goto finalize;
    }

    if (transfer->rc == NGX_OK) {
        if (!transfer->size_transfered) {
            if ((rc = ngx_http_video_thumbextractor_recv(c, ev, &transfer->buffer, sizeof(size_t))) != NGX_OK) {
                goto transfer_failed;
            }

            transfer->size_transfered = 1;

            if ((transfer->buffer.start = ngx_pcalloc(r->pool, transfer->size)) == NULL) {
                ngx_log_error(NGX_LOG_CRIT, r->connection->log, 0, "video thumb extractor module: unable to allocate buffer to receive the image");
                ngx_http_filter_finalize_request(r, &ngx_http_video_thumbextractor_module, NGX_HTTP_INTERNAL_SERVER_ERROR);
                goto finalize;
            }

            ngx_http_video_thumbextractor_set_buffer(&transfer->buffer, transfer->buffer.start, NULL, transfer->size);
            transfer->buffer.temporary = 1;

        }

        if ((rc = ngx_http_video_thumbextractor_recv(c, ev, &transfer->buffer, transfer->size)) != NGX_OK) {
            goto transfer_failed;
        }

        /* write response */
        r->headers_out.content_type = NGX_HTTP_VIDEO_THUMBEXTRACTOR_CONTENT_TYPE;
        r->headers_out.content_type_len = NGX_HTTP_VIDEO_THUMBEXTRACTOR_CONTENT_TYPE.len;
        r->headers_out.status = NGX_HTTP_OK;
        r->headers_out.content_length_n = transfer->size;

        out = (ngx_chain_t *) ngx_pcalloc(r->pool, sizeof(ngx_chain_t));
        if (out == NULL) {
            ngx_log_error(NGX_LOG_CRIT, r->connection->log, 0, "video thumb extractor module: unable to allocate output to send the image");
            goto finalize;
        }

        transfer->buffer.last_buf = 1;
        transfer->buffer.last_in_chain = 1;
        transfer->buffer.flush = 1;
        transfer->buffer.memory = 1;

        out->buf = &transfer->buffer;
        out->next = NULL;

        rc = ngx_http_video_thumbextractor_next_header_filter(r);
        if (rc == NGX_ERROR || rc > NGX_OK || r->header_only) {
            goto finalize;
        }
        ngx_http_video_thumbextractor_next_body_filter(r, out);
    }

transfer_failed:

    if (rc == NGX_AGAIN) {
        return;
    }

    if (rc == NGX_ERROR) {
        ngx_log_error(NGX_LOG_CRIT, r->connection->log, 0, "video thumb extractor module: error receiving data from extract thumbor process");
        ngx_http_filter_finalize_request(r, &ngx_http_video_thumbextractor_module, NGX_HTTP_INTERNAL_SERVER_ERROR);
    }

finalize:

    ngx_http_finalize_request(r, NGX_OK);

exit:

    ngx_close_connection(c);
    kill(ngx_http_video_thumbextractor_module_child_pids[ipc_ctx->slot], SIGKILL);
    ngx_http_video_thumbextractor_module_child_pids[ipc_ctx->slot] = -1;

    if (!ngx_queue_empty(ngx_http_video_thumbextractor_module_extract_queue)) {
        q = ngx_queue_head(ngx_http_video_thumbextractor_module_extract_queue);
        ipc_ctx = ngx_queue_data(q, ngx_http_video_thumbextractor_ipc_t, queue);
        vtmcf = ngx_http_get_module_main_conf(ipc_ctx->request, ngx_http_video_thumbextractor_module);
        ngx_http_video_thumbextractor_module_ensure_extractor_process(vtmcf);
    }
}


void
ngx_http_video_thumbextractor_extract_process_write_handler(ngx_event_t *ev)
{
    ngx_http_video_thumbextractor_transfer_t  *transfer;
    ngx_connection_t                          *c;
    ngx_int_t                                  rc;

    c = ev->data;
    transfer = c->data;

    ngx_http_video_thumbextractor_set_buffer(&transfer->buffer, transfer->buffer.start, transfer->buffer.last, 0);

    if (!transfer->rc_transfered) {
        if ((rc = ngx_http_video_thumbextractor_write(c, ev, &transfer->buffer, sizeof(ngx_int_t))) != NGX_OK) {
            goto transfer_failed;
        }

        ngx_http_video_thumbextractor_set_buffer(&transfer->buffer, (u_char *) &transfer->size, NULL, sizeof(size_t));
        transfer->rc_transfered = 1;
    }

    if (transfer->rc == NGX_OK) {
        if (!transfer->size_transfered) {
            if ((rc = ngx_http_video_thumbextractor_write(c, ev, &transfer->buffer, sizeof(size_t))) != NGX_OK) {
                goto transfer_failed;
            }

            ngx_http_video_thumbextractor_set_buffer(&transfer->buffer, (u_char *) transfer->data, NULL, transfer->size);
            transfer->size_transfered = 1;
        }

        rc = ngx_http_video_thumbextractor_write(c, ev, &transfer->buffer, transfer->size);
    }

transfer_failed:

    if (rc == NGX_AGAIN) {
        return;
    }

    if (rc == NGX_ERROR) {
        exit(-1);
    }

    exit(0);
}


ngx_int_t
ngx_http_video_thumbextractor_recv(ngx_connection_t *c, ngx_event_t *rev, ngx_buf_t *buf, ssize_t len)
{
    ssize_t size = len - (buf->last - buf->start);
    if (size == 0) {
        return NGX_OK;
    }

    ssize_t n = ngx_read_fd(c->fd, buf->last, size);

    if (n == NGX_AGAIN) {
        return NGX_AGAIN;
    }

    if ((n == NGX_ERROR) || (n == 0)) {
        return NGX_ERROR;
    }

    buf->last += n;

    if ((buf->last - buf->start) < len) {
        return NGX_AGAIN;
    }

    return NGX_OK;
}


ngx_int_t
ngx_http_video_thumbextractor_write(ngx_connection_t *c, ngx_event_t *wev, ngx_buf_t *buf, ssize_t len)
{
    ssize_t size = len - (buf->last - buf->start);
    if (size == 0) {
        return NGX_OK;
    }

    ssize_t n = ngx_write_fd(c->fd, buf->last, size);

    if (n == NGX_AGAIN) {
        return NGX_AGAIN;
    }

    if ((n == NGX_ERROR) || (n == 0)) {
        return NGX_ERROR;
    }

    buf->last += n;

    if ((buf->last - buf->start) < len) {
        return NGX_AGAIN;
    }

    return NGX_OK;
}


void
ngx_http_video_thumbextractor_set_buffer(ngx_buf_t *buf, u_char *start, u_char *last, ssize_t len)
{
    buf->start = start;
    buf->pos = buf->start;
    buf->last = (last != NULL) ? last : start;
    buf->end = len ? buf->start + len : buf->end;
    buf->temporary = 0;
    buf->memory = 1;
}

#include "common.h"

#define DEBUG_FLAG SEAFILE_DEBUG_HTTP
#include "log.h"

#if defined(__FreeBSD__) || defined(__NetBSD__) || defined(__OpenBSD__)
#include <event2/event.h>
#include <event2/bufferevent.h>
#include <event2/bufferevent_struct.h>
#else
#include <event.h>
#endif

#include <evhtp.h>

#include <sys/stat.h>
#include <sys/types.h>
#include <fcntl.h>

#include "seafile-object.h"
#include "seafile-crypt.h"

#include "utils.h"

#include "seafile-session.h"
#include "access-file.h"
#include "zip-download-mgr.h"
#include "http-server.h"

#define FILE_TYPE_MAP_DEFAULT_LEN 1
#define BUFFER_SIZE 1024 * 64
#define MULTI_DOWNLOAD_FILE_PREFIX "documents-export-"

struct file_type_map {
    char *suffix;
    char *type;
};

typedef struct SendBlockData {
    evhtp_request_t *req;
    char *block_id;
    BlockHandle *handle;
    uint32_t bsize;
    uint32_t remain;

    char store_id[37];
    int repo_version;

    char *user;

    bufferevent_data_cb saved_read_cb;
    bufferevent_data_cb saved_write_cb;
    bufferevent_event_cb saved_event_cb;
    void *saved_cb_arg;
} SendBlockData;

typedef struct SendfileData {
    evhtp_request_t *req;
    Seafile *file;
    SeafileCrypt *crypt;
    gboolean enc_init;
    EVP_CIPHER_CTX *ctx;
    BlockHandle *handle;
    size_t remain;
    int idx;

    char store_id[37];
    int repo_version;

    char *user;
    char *token_type;

    bufferevent_data_cb saved_read_cb;
    bufferevent_data_cb saved_write_cb;
    bufferevent_event_cb saved_event_cb;
    void *saved_cb_arg;
} SendfileData;

typedef struct SendFileRangeData {
    evhtp_request_t *req;
    Seafile *file;
    BlockHandle *handle;
    int blk_idx;
    guint64 start_off;
    guint64 range_remain;

    char store_id[37];
    int repo_version;

    char *user;
    char *token_type;

    bufferevent_data_cb saved_read_cb;
    bufferevent_data_cb saved_write_cb;
    bufferevent_event_cb saved_event_cb;
    void *saved_cb_arg;
} SendFileRangeData;

typedef struct SendDirData {
    evhtp_request_t *req;
    size_t remain;
    guint64 total_size;

    int zipfd;
    char *zipfile;
    char *token;
    char *user;
    char *token_type;
    char repo_id[37];

    bufferevent_data_cb saved_read_cb;
    bufferevent_data_cb saved_write_cb;
    bufferevent_event_cb saved_event_cb;
    void *saved_cb_arg;
} SendDirData;



extern SeafileSession *seaf;

static struct file_type_map ftmap[] = {
    { "txt", "text/plain" },
    { "doc", "application/vnd.ms-word" },
    { "docx", "application/vnd.openxmlformats-officedocument.wordprocessingml.document" },
    { "ppt", "application/vnd.ms-powerpoint" },
    { "pptx", "application/vnd.openxmlformats-officedocument.presentationml.presentation" },
    { "xls", "application/vnd.ms-excel" },
    { "xlsx", "application/vnd.openxmlformats-officedocument.spreadsheetml.sheet" },
    { "pdf", "application/pdf" },
    { "zip", "application/zip"},
    { "mp3", "audio/mp3" },
    { "mpeg", "video/mpeg" },
    { "mp4", "video/mp4" },
    { "jpg", "image/jpeg" },
    { "JPG", "image/jpeg" },
    { "jpeg", "image/jpeg" },
    { "JPEG", "image/jpeg" },
    { "png", "image/png" },
    { "PNG", "image/png" },
    { "gif", "image/gif" },
    { "GIF", "image/gif" },
    { "svg", "image/svg+xml" },
    { "SVG", "image/svg+xml" },
    { NULL, NULL },
};

static void
free_sendblock_data (SendBlockData *data)
{
    if (data->handle) {
        seaf_block_manager_close_block(seaf->block_mgr, data->handle);
        seaf_block_manager_block_handle_free(seaf->block_mgr, data->handle);
    }

    g_free (data->block_id);
    g_free (data->user);
    g_free (data);
}

static void
free_sendfile_data (SendfileData *data)
{
    if (data->handle) {
        seaf_block_manager_close_block(seaf->block_mgr, data->handle);
        seaf_block_manager_block_handle_free(seaf->block_mgr, data->handle);
    }

    if (data->enc_init)
        EVP_CIPHER_CTX_free (data->ctx);

    seafile_unref (data->file);
    g_free (data->user);
    g_free (data->token_type);
    g_free (data->crypt);
    g_free (data);
}

static void
free_send_file_range_data (SendFileRangeData *data)
{
    if (data->handle) {
        seaf_block_manager_close_block(seaf->block_mgr, data->handle);
        seaf_block_manager_block_handle_free(seaf->block_mgr, data->handle);
    }

    seafile_unref (data->file);
    g_free (data->user);
    g_free (data->token_type);
    g_free (data);
}

static void
free_senddir_data (SendDirData *data)
{
    close (data->zipfd);

    zip_download_mgr_del_zip_progress (seaf->zip_download_mgr, data->token);

    g_free (data->user);
    g_free (data->token_type);
    g_free (data->token);
    g_free (data);
}

static void
write_block_data_cb (struct bufferevent *bev, void *ctx)
{
    SendBlockData *data = ctx;
    char *blk_id;
    BlockHandle *handle;
    char buf[1024 * 64];
    int n;

    blk_id = data->block_id;

    if (!data->handle) {
        data->handle = seaf_block_manager_open_block(seaf->block_mgr,
                                                     data->store_id,
                                                     data->repo_version,
                                                     blk_id, BLOCK_READ);
        if (!data->handle) {
            seaf_warning ("Failed to open block %s:%s\n", data->store_id, blk_id);
            goto err;
        }

        data->remain = data->bsize;
    }
    handle = data->handle;

    n = seaf_block_manager_read_block(seaf->block_mgr, handle, buf, sizeof(buf));
    data->remain -= n;
    if (n < 0) {
        seaf_warning ("Error when reading from block %s:%s.\n",
                      data->store_id, blk_id);
        goto err;
    } else if (n == 0) {
        /* We've read up the data of this block, finish. */
        seaf_block_manager_close_block (seaf->block_mgr, handle);
        seaf_block_manager_block_handle_free (seaf->block_mgr, handle);
        data->handle = NULL;

        /* Recover evhtp's callbacks */
        bev->readcb = data->saved_read_cb;
        bev->writecb = data->saved_write_cb;
        bev->errorcb = data->saved_event_cb;
        bev->cbarg = data->saved_cb_arg;

        /* Resume reading incomming requests. */
        evhtp_request_resume (data->req);

        evhtp_send_reply_end (data->req);

        send_statistic_msg (data->store_id, data->user, "web-file-download", (guint64)data->bsize);

        free_sendblock_data (data);
        return;
    }

    /* OK, we've got some data to send. */
    bufferevent_write (bev, buf, n);

    return;

err:
    evhtp_connection_free (evhtp_request_get_connection (data->req));
    free_sendblock_data (data);
    return;
}

static void
write_data_cb (struct bufferevent *bev, void *ctx)
{
    SendfileData *data = ctx;
    char *blk_id;
    BlockHandle *handle;
    char buf[1024 * 64];
    int n;

next:
    blk_id = data->file->blk_sha1s[data->idx];

    if (!data->handle) {
        data->handle = seaf_block_manager_open_block(seaf->block_mgr,
                                                     data->store_id,
                                                     data->repo_version,
                                                     blk_id, BLOCK_READ);
        if (!data->handle) {
            seaf_warning ("Failed to open block %s:%s\n", data->store_id, blk_id);
            goto err;
        }

        BlockMetadata *bmd;
        bmd = seaf_block_manager_stat_block_by_handle (seaf->block_mgr,
                                                       data->handle);
        if (!bmd)
            goto err;
        data->remain = bmd->size;
        g_free (bmd);

        if (data->crypt) {
            if (seafile_decrypt_init (&data->ctx,
                                      data->crypt->version,
                                      (unsigned char *)data->crypt->key,
                                      (unsigned char *)data->crypt->iv) < 0) {
                seaf_warning ("Failed to init decrypt.\n");
                goto err;
            }
            data->enc_init = TRUE;
        }
    }
    handle = data->handle;

    n = seaf_block_manager_read_block(seaf->block_mgr, handle, buf, sizeof(buf));
    data->remain -= n;
    if (n < 0) {
        seaf_warning ("Error when reading from block %s.\n", blk_id);
        goto err;
    } else if (n == 0) {
        /* We've read up the data of this block, finish or try next block. */
        seaf_block_manager_close_block (seaf->block_mgr, handle);
        seaf_block_manager_block_handle_free (seaf->block_mgr, handle);
        data->handle = NULL;
        if (data->crypt != NULL) {
            EVP_CIPHER_CTX_free (data->ctx);
            data->enc_init = FALSE;
        }

        if (data->idx == data->file->n_blocks - 1) {
            /* Recover evhtp's callbacks */
            bev->readcb = data->saved_read_cb;
            bev->writecb = data->saved_write_cb;
            bev->errorcb = data->saved_event_cb;
            bev->cbarg = data->saved_cb_arg;

            /* Resume reading incomming requests. */
            evhtp_request_resume (data->req);

            evhtp_send_reply_end (data->req);

            if (g_strcmp0(data->token_type, "view") != 0) {
                char *oper = "web-file-download";
                if (g_strcmp0(data->token_type, "download-link") == 0)
                    oper = "link-file-download";

                send_statistic_msg(data->store_id, data->user, oper,
                                   (guint64)data->file->file_size);
            }

            free_sendfile_data (data);
            return;
        }

        ++(data->idx);
        goto next;
    }

    /* OK, we've got some data to send. */
    if (data->crypt != NULL) {
        char *dec_out;
        int dec_out_len = -1;
        struct evbuffer *tmp_buf;

        dec_out = g_new (char, n + 16);
        if (!dec_out) {
            seaf_warning ("Failed to alloc memory.\n");
            goto err;
        }

        int ret = EVP_DecryptUpdate (data->ctx,
                                     (unsigned char *)dec_out,
                                     &dec_out_len,
                                     (unsigned char *)buf,
                                     n);
        if (ret == 0) {
            seaf_warning ("Decrypt block %s:%s failed.\n", data->store_id, blk_id);
            g_free (dec_out);
            goto err;
        }

        tmp_buf = evbuffer_new ();

        evbuffer_add (tmp_buf, dec_out, dec_out_len);

        /* If it's the last piece of a block, call decrypt_final()
         * to decrypt the possible partial block. */
        if (data->remain == 0) {
            ret = EVP_DecryptFinal_ex (data->ctx,
                                       (unsigned char *)dec_out,
                                       &dec_out_len);
            if (ret == 0) {
                seaf_warning ("Decrypt block %s:%s failed.\n", data->store_id, blk_id);
                evbuffer_free (tmp_buf);
                g_free (dec_out);
                goto err;
            }
            evbuffer_add (tmp_buf, dec_out, dec_out_len);
        }
        /* This may call write_data_cb() recursively (by libevent_openssl).
         * SendfileData struct may be free'd in the recursive calls.
         * So don't use "data" variable after here.
         */
        bufferevent_write_buffer (bev, tmp_buf);

        evbuffer_free (tmp_buf);
        g_free (dec_out);
    } else {
        bufferevent_write (bev, buf, n);
    }

    return;

err:
    evhtp_connection_free (evhtp_request_get_connection (data->req));
    free_sendfile_data (data);
    return;
}

static void
write_dir_data_cb (struct bufferevent *bev, void *ctx)
{
    SendDirData *data = ctx;
    char buf[64 * 1024];
    int n;

    n = readn (data->zipfd, buf, sizeof(buf));
    if (n < 0) {
        seaf_warning ("Failed to read zipfile %s: %s.\n", data->zipfile, strerror (errno));
        evhtp_connection_free (evhtp_request_get_connection (data->req));
        free_senddir_data (data);
    } else if (n > 0) {
        bufferevent_write (bev, buf, n);
        data->remain -= n;

        if (data->remain == 0) {
            /* Recover evhtp's callbacks */
            bev->readcb = data->saved_read_cb;
            bev->writecb = data->saved_write_cb;
            bev->errorcb = data->saved_event_cb;
            bev->cbarg = data->saved_cb_arg;

            /* Resume reading incomming requests. */
            evhtp_request_resume (data->req);

            evhtp_send_reply_end (data->req);

            char *oper = "web-file-download";
            if (g_strcmp0(data->token_type, "download-dir-link") == 0 ||
                g_strcmp0(data->token_type, "download-multi-link") == 0)
                oper = "link-file-download";

            send_statistic_msg(data->repo_id, data->user, oper, data->total_size);

            free_senddir_data (data);
            return;
        }
    }
}

static void
my_block_event_cb (struct bufferevent *bev, short events, void *ctx)
{
    SendBlockData *data = ctx;

    data->saved_event_cb (bev, events, data->saved_cb_arg);

    /* Free aux data. */
    free_sendblock_data (data);
}

static void
my_event_cb (struct bufferevent *bev, short events, void *ctx)
{
    SendfileData *data = ctx;

    data->saved_event_cb (bev, events, data->saved_cb_arg);

    /* Free aux data. */
    free_sendfile_data (data);
}

static void
file_range_event_cb (struct bufferevent *bev, short events, void *ctx)
{
    SendFileRangeData *data = ctx;

    data->saved_event_cb (bev, events, data->saved_cb_arg);

    /* Free aux data. */
    free_send_file_range_data (data);
}

static void
my_dir_event_cb (struct bufferevent *bev, short events, void *ctx)
{
    SendDirData *data = ctx;

    data->saved_event_cb (bev, events, data->saved_cb_arg);

    /* Free aux data. */
    free_senddir_data (data);
}

static char *
parse_content_type(const char *filename)
{
    char *p;
    int i;

    if ((p = strrchr(filename, '.')) == NULL)
        return NULL;
    p++;

    for (i = 0; ftmap[i].suffix != NULL; i++) {
        if (strcmp(p, ftmap[i].suffix) == 0)
            return ftmap[i].type;
    }

    return NULL;
}

static gboolean
test_firefox (evhtp_request_t *req)
{
    const char *user_agent = evhtp_header_find (req->headers_in, "User-Agent");
    if (!user_agent)
        return FALSE;

    GString *s = g_string_new (user_agent);
    if (g_strrstr (g_string_ascii_down (s)->str, "firefox")) {
        g_string_free (s, TRUE);
        return TRUE;
    }
    else {
        g_string_free (s, TRUE);
        return FALSE;
    }
}

static int
do_file(evhtp_request_t *req, SeafRepo *repo, const char *file_id,
        const char *filename, const char *operation,
        SeafileCryptKey *crypt_key, const char *user)
{
    Seafile *file;
    char *type = NULL;
    char file_size[255];
    gchar *content_type = NULL;
    char cont_filename[SEAF_PATH_MAX];
    char *key_hex, *iv_hex;
    unsigned char enc_key[32], enc_iv[16];
    SeafileCrypt *crypt = NULL;
    SendfileData *data;
    char *policy = "sandbox";

    file = seaf_fs_manager_get_seafile(seaf->fs_mgr,
                                       repo->store_id, repo->version, file_id);
    if (file == NULL)
        return -1;

    if (crypt_key != NULL) {
        g_object_get (crypt_key,
                      "key", &key_hex,
                      "iv", &iv_hex,
                      NULL);
        if (repo->enc_version == 1)
            hex_to_rawdata (key_hex, enc_key, 16);
        else
            hex_to_rawdata (key_hex, enc_key, 32);
        hex_to_rawdata (iv_hex, enc_iv, 16);
        crypt = seafile_crypt_new (repo->enc_version, enc_key, enc_iv);
        g_free (key_hex);
        g_free (iv_hex);
    }

    evhtp_headers_add_header(req->headers_out,
                             evhtp_header_new("Access-Control-Allow-Origin",
                                              "*", 1, 1));

    evhtp_headers_add_header(req->headers_out,
                             evhtp_header_new("Content-Security-Policy",
                                              policy, 1, 1));

    type = parse_content_type(filename);
    if (type != NULL) {
        if (strstr(type, "text")) {
            content_type = g_strjoin("; ", type, "charset=gbk", NULL);
        } else {
            content_type = g_strdup (type);
        }

        evhtp_headers_add_header(req->headers_out,
                                 evhtp_header_new("Content-Type",
                                                  content_type, 1, 1));
        g_free (content_type);
    } else
        evhtp_headers_add_header (req->headers_out,
                                  evhtp_header_new("Content-Type",
                                                   "application/octet-stream", 1, 1));

    snprintf(file_size, sizeof(file_size), "%"G_GINT64_FORMAT"", file->file_size);
    evhtp_headers_add_header (req->headers_out,
                              evhtp_header_new("Content-Length", file_size, 1, 1));

    if (strcmp(operation, "download") == 0 ||
        strcmp(operation, "download-link") == 0) {
        /* Safari doesn't support 'utf8', 'utf-8' is compatible with most of browsers. */
        snprintf(cont_filename, SEAF_PATH_MAX,
                 "attachment;filename*=\"utf-8\' \'%s\"", filename);
    } else {
        if (test_firefox (req)) {
            snprintf(cont_filename, SEAF_PATH_MAX,
                     "inline;filename*=\"utf-8\' \'%s\"", filename);
        } else {
            snprintf(cont_filename, SEAF_PATH_MAX,
                     "inline;filename=\"%s\"", filename);
        }
    }
    evhtp_headers_add_header(req->headers_out,
                             evhtp_header_new("Content-Disposition", cont_filename,
                                              1, 1));

    if (g_strcmp0 (type, "image/jpg") != 0) {
        evhtp_headers_add_header(req->headers_out,
                                 evhtp_header_new("X-Content-Type-Options", "nosniff",
                                                  1, 1));
    }
    /* HEAD Request */
    if (evhtp_request_get_method(req) == htp_method_HEAD) {
        evhtp_send_reply (req, EVHTP_RES_OK);
        return 0;
    }

    /* If it's an empty file, send an empty reply. */
    if (file->n_blocks == 0) {
        evhtp_send_reply (req, EVHTP_RES_OK);
        seafile_unref (file);
        return 0;
    }

    data = g_new0 (SendfileData, 1);
    data->req = req;
    data->file = file;
    data->crypt = crypt;
    data->user = g_strdup(user);
    data->token_type = g_strdup (operation);

    memcpy (data->store_id, repo->store_id, 36);
    data->repo_version = repo->version;

    /* We need to overwrite evhtp's callback functions to
     * write file data piece by piece.
     */
    struct bufferevent *bev = evhtp_request_get_bev (req);
    data->saved_read_cb = bev->readcb;
    data->saved_write_cb = bev->writecb;
    data->saved_event_cb = bev->errorcb;
    data->saved_cb_arg = bev->cbarg;
    bufferevent_setcb (bev,
                       NULL,
                       write_data_cb,
                       my_event_cb,
                       data);
    /* Block any new request from this connection before finish
     * handling this request.
     */
    evhtp_request_pause (req);

    /* Kick start data transfer by sending out http headers. */
    evhtp_send_reply_start(req, EVHTP_RES_OK);

    return 0;
}

// get block handle for range start
static BlockHandle *
get_start_block_handle (const char *store_id, int version, Seafile *file,
                        guint64 start, int *blk_idx)
{
    BlockHandle *handle = NULL;
    BlockMetadata *bmd;
    char *blkid;
    guint64 tolsize = 0;
    int i = 0;

    for (; i < file->n_blocks; i++) {
        blkid = file->blk_sha1s[i];

        bmd = seaf_block_manager_stat_block(seaf->block_mgr, store_id,
                                            version, blkid);
        if (!bmd)
            return NULL;

        if (start < tolsize + bmd->size) {
            g_free (bmd);
            break;
        }
        tolsize += bmd->size;
        g_free (bmd);
    }

    /* beyond the file size */
    if (i == file->n_blocks)
        return NULL;

    handle = seaf_block_manager_open_block(seaf->block_mgr,
                                           store_id, version,
                                           blkid, BLOCK_READ);
    if (!handle) {
        seaf_warning ("Failed to open block %s:%s.\n", store_id, blkid);
        return NULL;
    }

    /* trim the offset in a block */
    if (start > tolsize) {
        char *tmp = (char *)malloc(sizeof(*tmp) * (start - tolsize));
        if (!tmp)
            goto err;

        int n = seaf_block_manager_read_block(seaf->block_mgr, handle,
                                              tmp, start-tolsize);
        if (n != start-tolsize) {
            seaf_warning ("Failed to read block %s:%s.\n", store_id, blkid);
            free (tmp);
            goto err;
        }
        free (tmp);
    }

    *blk_idx = i;
    return handle;

err:
    seaf_block_manager_close_block(seaf->block_mgr, handle);
    seaf_block_manager_block_handle_free (seaf->block_mgr, handle);
    return NULL;
}

static void
finish_file_range_request (struct bufferevent *bev, SendFileRangeData *data)
{
    /* Recover evhtp's callbacks */
    bev->readcb = data->saved_read_cb;
    bev->writecb = data->saved_write_cb;
    bev->errorcb = data->saved_event_cb;
    bev->cbarg = data->saved_cb_arg;

    /* Resume reading incomming requests. */
    evhtp_request_resume (data->req);

    evhtp_send_reply_end (data->req);

    free_send_file_range_data (data);
}

static void
write_file_range_cb (struct bufferevent *bev, void *ctx)
{
    SendFileRangeData *data = ctx;
    char *blk_id;
    char buf[BUFFER_SIZE];
    int bsize;
    int n;

    if (data->blk_idx == -1) {
        // start to send block
        data->handle = get_start_block_handle (data->store_id, data->repo_version,
                                               data->file, data->start_off,
                                               &data->blk_idx);
        if (!data->handle)
            goto err;
    }

next:
    blk_id = data->file->blk_sha1s[data->blk_idx];

    if (!data->handle) {
        data->handle = seaf_block_manager_open_block(seaf->block_mgr,
                                                     data->store_id,
                                                     data->repo_version,
                                                     blk_id, BLOCK_READ);
        if (!data->handle) {
            seaf_warning ("Failed to open block %s:%s\n", data->store_id, blk_id);
            goto err;
        }
    }

    bsize = data->range_remain < BUFFER_SIZE ? data->range_remain : BUFFER_SIZE;
    n = seaf_block_manager_read_block(seaf->block_mgr, data->handle, buf, bsize);
    data->range_remain -= n;
    if (n < 0) {
        seaf_warning ("Error when reading from block %s:%s.\n",
                      data->store_id, blk_id);
        goto err;
    } else if (n == 0) {
        seaf_block_manager_close_block (seaf->block_mgr, data->handle);
        seaf_block_manager_block_handle_free (seaf->block_mgr, data->handle);
        data->handle = NULL;
        ++data->blk_idx;
        goto next;
    }

    bufferevent_write (bev, buf, n);
    if (data->range_remain == 0) {
        if (data->start_off + n >= data->file->file_size) {
            char *oper = "web-file-download";
            if (g_strcmp0(data->token_type, "download-link") == 0)
                oper = "link-file-download";

            send_statistic_msg (data->store_id, data->user, oper,
                                (guint64)data->file->file_size);
        }
        finish_file_range_request (bev, data);
    }

    return;

err:
    evhtp_connection_free (evhtp_request_get_connection (data->req));
    free_send_file_range_data (data);
}

// parse range offset, only support single range (-num, num-num, num-)
static gboolean
parse_range_val (const char *byte_ranges, guint64 *pstart, guint64 *pend,
                 guint64 fsize)
{
    char *minus;
    char *end_ptr;
    gboolean error = FALSE;
    char *ranges_dup = g_strdup (strchr(byte_ranges, '=') + 1);
    char *tmp = ranges_dup;
    guint64 start;
    guint64 end;

    minus = strchr(tmp, '-');
    if (!minus)
        return FALSE;

    if (minus == tmp) {
        // -num mode
        start = strtoll(tmp, &end_ptr, 10);
        if (start == 0) {
            // range format is invalid
            error = TRUE;
        } else if (*end_ptr == '\0') {
            end = fsize - 1;
            start += fsize;
        } else {
            error = TRUE;
        }
    } else if (*(minus + 1) == '\0') {
        // num- mode
        start = strtoll(tmp, &end_ptr, 10);
        if (end_ptr == minus) {
            end = fsize - 1;
        } else {
            error = TRUE;
        }
    } else {
        // num-num mode
        start = strtoll(tmp, &end_ptr, 10);
        if (end_ptr == minus) {
            end = strtoll(minus + 1, &end_ptr, 10);
            if (*end_ptr != '\0') {
                error = TRUE;
            }
        } else {
            error = TRUE;
        }
    }

    g_free (ranges_dup);

    if (error)
        return FALSE;

    if (end > fsize - 1) {
        end = fsize - 1;
    }
    if (start > end) {
        // Range format is valid, but range number is invalid
        return FALSE;
    }

    *pstart = start;
    *pend = end;

    return TRUE;
}

static void
set_resp_disposition (evhtp_request_t *req, const char *operation,
                      const char *filename)
{
    char *cont_filename = NULL;

    if (strcmp(operation, "download") == 0) {
        if (test_firefox (req)) {
            cont_filename = g_strdup_printf("attachment;filename*=\"utf-8\' \'%s\"",
                                            filename);

        } else {
            cont_filename = g_strdup_printf("attachment;filename=\"%s\"", filename);
        }
    } else {
        if (test_firefox (req)) {
            cont_filename = g_strdup_printf("inline;filename*=\"utf-8\' \'%s\"",
                                            filename);
        } else {
            cont_filename = g_strdup_printf("inline;filename=\"%s\"", filename);
        }
    }

    evhtp_headers_add_header(req->headers_out,
                             evhtp_header_new("Content-Disposition", cont_filename,
                                              0, 1));
    g_free (cont_filename);
}

static int
do_file_range (evhtp_request_t *req, SeafRepo *repo, const char *file_id,
               const char *filename, const char *operation, const char *byte_ranges,
               const char *user)
{
    Seafile *file;
    SendFileRangeData *data = NULL;
    guint64 start;
    guint64 end;
    char *policy = "sandbox";

    file = seaf_fs_manager_get_seafile(seaf->fs_mgr,
                                       repo->store_id, repo->version, file_id);
    if (file == NULL)
        return -1;

    /* If it's an empty file, send an empty reply. */
    if (file->n_blocks == 0) {
        evhtp_send_reply (req, EVHTP_RES_OK);
        seafile_unref (file);
        return 0;
    }

    if (!parse_range_val (byte_ranges, &start, &end, file->file_size)) {
        seafile_unref (file);
        char *con_range = g_strdup_printf ("bytes */%"G_GUINT64_FORMAT, file->file_size);
        evhtp_headers_add_header (req->headers_out,
                                  evhtp_header_new("Content-Range", con_range,
                                                   0, 1));
        g_free (con_range);
        evhtp_send_reply (req, EVHTP_RES_RANGENOTSC);
        return 0;
    }

    evhtp_headers_add_header (req->headers_out,
                              evhtp_header_new ("Accept-Ranges", "bytes", 0, 0));

    evhtp_headers_add_header(req->headers_out,
                             evhtp_header_new("Content-Security-Policy",
                                              policy, 1, 1));

    char *content_type = NULL;
    char *type = parse_content_type (filename);
    if (type != NULL) {
        if (strstr(type, "text")) {
            content_type = g_strjoin("; ", type, "charset=gbk", NULL);
        } else {
            content_type = g_strdup (type);
        }
    } else {
        content_type = g_strdup ("application/octet-stream");
    }

    evhtp_headers_add_header (req->headers_out,
                              evhtp_header_new ("Content-Type", content_type, 0, 1));
    g_free (content_type);

    char *con_len = g_strdup_printf ("%"G_GUINT64_FORMAT, end-start+1);
    evhtp_headers_add_header (req->headers_out,
                              evhtp_header_new("Content-Length", con_len, 0, 1));
    g_free (con_len);

    char *con_range = g_strdup_printf ("%s %"G_GUINT64_FORMAT"-%"G_GUINT64_FORMAT
                                       "/%"G_GUINT64_FORMAT, "bytes",
                                       start, end, file->file_size);
    evhtp_headers_add_header (req->headers_out,
                              evhtp_header_new ("Content-Range", con_range, 0, 1));
    g_free (con_range);

    set_resp_disposition (req, operation, filename);

    if (g_strcmp0 (type, "image/jpg") != 0) {
        evhtp_headers_add_header(req->headers_out,
                                 evhtp_header_new("X-Content-Type-Options", "nosniff",
                                                  1, 1));
    }

    data = g_new0 (SendFileRangeData, 1);
    if (!data) {
        seafile_unref (file);
        return -1;
    }
    data->req = req;
    data->file = file;
    data->blk_idx = -1;
    data->start_off = start;
    data->range_remain = end-start+1;
    data->user = g_strdup(user);
    data->token_type = g_strdup (operation);

    memcpy (data->store_id, repo->store_id, 36);
    data->repo_version = repo->version;

    /* We need to overwrite evhtp's callback functions to
     * write file data piece by piece.
     */
    struct bufferevent *bev = evhtp_request_get_bev (req);
    data->saved_read_cb = bev->readcb;
    data->saved_write_cb = bev->writecb;
    data->saved_event_cb = bev->errorcb;
    data->saved_cb_arg = bev->cbarg;
    bufferevent_setcb (bev,
                       NULL,
                       write_file_range_cb,
                       file_range_event_cb,
                       data);


    /* Block any new request from this connection before finish
     * handling this request.
     */
    evhtp_request_pause (req);

    /* Kick start data transfer by sending out http headers. */
    evhtp_send_reply_start(req, EVHTP_RES_PARTIAL);

    return 0;
}

static int
start_download_zip_file (evhtp_request_t *req, const char *token,
                         const char *zipname, char *zipfile,
                         const char *repo_id, const char *user, const char *token_type)
{
    SeafStat st;
    char file_size[255];
    char cont_filename[SEAF_PATH_MAX];
    int zipfd = 0;

    if (seaf_stat(zipfile, &st) < 0) {
        seaf_warning ("Failed to stat %s: %s.\n", zipfile, strerror(errno));
        return -1;
    }

    evhtp_headers_add_header(req->headers_out,
                             evhtp_header_new("Content-Type", "application/zip", 1, 1));

    snprintf (file_size, sizeof(file_size), "%"G_GUINT64_FORMAT"", st.st_size);
    evhtp_headers_add_header (req->headers_out,
            evhtp_header_new("Content-Length", file_size, 1, 1));

    snprintf(cont_filename, SEAF_PATH_MAX,
             "attachment;filename=\"%s.zip\"", zipname);

    evhtp_headers_add_header(req->headers_out,
            evhtp_header_new("Content-Disposition", cont_filename, 1, 1));

    zipfd = g_open (zipfile, O_RDONLY | O_BINARY, 0);
    if (zipfd < 0) {
        seaf_warning ("Failed to open zipfile %s: %s.\n", zipfile, strerror(errno));
        return -1;
    }

    SendDirData *data;
    data = g_new0 (SendDirData, 1);
    data->req = req;
    data->zipfd = zipfd;
    data->zipfile = zipfile;
    data->token = g_strdup (token);
    data->remain = st.st_size;
    data->total_size = (guint64)st.st_size;
    data->user = g_strdup (user);
    data->token_type = g_strdup (token_type);
    snprintf(data->repo_id, sizeof(data->repo_id), "%s", repo_id);

    /* We need to overwrite evhtp's callback functions to
     * write file data piece by piece.
     */
    struct bufferevent *bev = evhtp_request_get_bev (req);
    data->saved_read_cb = bev->readcb;
    data->saved_write_cb = bev->writecb;
    data->saved_event_cb = bev->errorcb;
    data->saved_cb_arg = bev->cbarg;
    bufferevent_setcb (bev,
                       NULL,
                       write_dir_data_cb,
                       my_dir_event_cb,
                       data);
    /* Block any new request from this connection before finish
     * handling this request.
     */
    evhtp_request_pause (req);

    /* Kick start data transfer by sending out http headers. */
    evhtp_send_reply_start(req, EVHTP_RES_OK);

    return 0;
}

static gboolean
can_use_cached_content (evhtp_request_t *req)
{
    if (evhtp_kv_find (req->headers_in, "If-Modified-Since") != NULL) {
        evhtp_send_reply (req, EVHTP_RES_NOTMOD);
        return TRUE;
    }

    char http_date[256];
    evhtp_kv_t *kv;
    time_t now = time(NULL);

    /* Set Last-Modified header if the client gets this file
     * for the first time. So that the client will set
     * If-Modified-Since header the next time it gets the same
     * file.
     */
#ifndef WIN32
    strftime (http_date, sizeof(http_date), "%a, %d %b %Y %T GMT",
              gmtime(&now));
#else
    strftime (http_date, sizeof(http_date), "%a, %d %b %Y %H:%M:%S GMT",
              gmtime(&now));
#endif
    kv = evhtp_kv_new ("Last-Modified", http_date, 1, 1);
    evhtp_kvs_add_kv (req->headers_out, kv);

    kv = evhtp_kv_new ("Cache-Control", "max-age=3600", 1, 1);
    evhtp_kvs_add_kv (req->headers_out, kv);

    return FALSE;
}

static void
access_zip_cb (evhtp_request_t *req, void *arg)
{
    char *token;
    SeafileWebAccess *info = NULL;
    char *info_str = NULL;
    json_t *info_obj = NULL;
    json_error_t jerror;
    char *filename = NULL;
    char *repo_id = NULL;
    char *user = NULL;
    char *zip_file_path;
    char *token_type = NULL;
    const char *error = NULL;
    int error_code;

    char **parts = g_strsplit (req->uri->path->full + 1, "/", 0);
    if (g_strv_length (parts) != 2) {
        error = "Invalid URL\n";
        error_code = EVHTP_RES_BADREQ;
        goto out;
    }

    token = parts[1];
    info = seaf_web_at_manager_query_access_token (seaf->web_at_mgr, token);
    // Here only check token exist, follow will get zip file path, if zip file path exist
    // then the token is valid, because it pass some validations in zip stage
    if (!info) {
        error = "Access token not found\n";
        error_code = EVHTP_RES_FORBIDDEN;
        goto out;
    }

    g_object_get (info, "obj_id", &info_str, NULL);
    if (!info_str) {
        seaf_warning ("Invalid obj_id for token: %s.\n", token);
        error_code = EVHTP_RES_SERVERR;
        goto out;
    }

    info_obj = json_loadb (info_str, strlen(info_str), 0, &jerror);
    if (!info_obj) {
        seaf_warning ("Failed to parse obj_id field: %s for token: %s.\n", jerror.text, token);
        error_code = EVHTP_RES_SERVERR;
        goto out;
    }

    if (json_object_has_member (info_obj, "dir_name")) {
        // Download dir
        filename = g_strdup (json_object_get_string_member (info_obj, "dir_name"));
    } else if (json_object_has_member (info_obj, "file_list")) {
        // Download multi
        time_t now = time(NULL);
        char date_str[11];
        strftime(date_str, sizeof(date_str), "%Y-%m-%d", localtime(&now));
        filename = g_strconcat (MULTI_DOWNLOAD_FILE_PREFIX, date_str, NULL);
    } else {
        seaf_warning ("No dir_name or file_list in obj_id for token: %s.\n", token);
        error_code = EVHTP_RES_SERVERR;
        goto out;
    }

    zip_file_path = zip_download_mgr_get_zip_file_path (seaf->zip_download_mgr, token);
    if (!zip_file_path) {
        g_object_get (info, "repo_id", &repo_id, NULL);
        seaf_warning ("Failed to get zip file path for %s in repo %.8s, token:[%s].\n",
                      filename, repo_id, token);
        error_code = EVHTP_RES_SERVERR;
        goto out;
    }

    if (can_use_cached_content (req)) {
        // Clean zip progress related resource
        zip_download_mgr_del_zip_progress (seaf->zip_download_mgr, token);
        goto out;
    }

    g_object_get (info, "username", &user, NULL);
    g_object_get (info, "repo_id", &repo_id, NULL);
    g_object_get (info, "op", &token_type, NULL);
    int ret = start_download_zip_file (req, token, filename, zip_file_path, repo_id, user, token_type);
    if (ret < 0) {
        seaf_warning ("Failed to start download zip file: %s for token: %s", filename, token);
        error_code = EVHTP_RES_SERVERR;
    }

out:
    g_strfreev (parts);
    if (info)
        g_object_unref (info);
    if (info_str)
        g_free (info_str);
    if (info_obj)
        json_decref (info_obj);
    if (filename)
        g_free (filename);
    if (repo_id)
        g_free (repo_id);
    if (user)
        g_free (user);
    if (token_type)
        g_free (token_type);

    if (error) {
        evbuffer_add_printf(req->buffer_out, "%s\n", error);
        evhtp_send_reply(req, error_code);
    }
}

static void
access_cb(evhtp_request_t *req, void *arg)
{
    SeafRepo *repo = NULL;
    char *error = NULL;
    char *token = NULL;
    char *filename = NULL;
    const char *repo_id = NULL;
    const char *data = NULL;
    const char *operation = NULL;
    const char *user = NULL;
    const char *byte_ranges = NULL;
    int error_code = EVHTP_RES_BADREQ;

    SeafileCryptKey *key = NULL;
    SeafileWebAccess *webaccess = NULL;

    /* Skip the first '/'. */
    char **parts = g_strsplit (req->uri->path->full + 1, "/", 0);
    if (!parts || g_strv_length (parts) < 3 ||
        strcmp (parts[0], "files") != 0) {
        error = "Invalid URL";
        goto on_error;
    }

    token = parts[1];
    filename = parts[2];

    webaccess = seaf_web_at_manager_query_access_token (seaf->web_at_mgr, token);
    if (!webaccess) {
        error = "Access token not found";
        error_code = EVHTP_RES_FORBIDDEN;
        goto on_error;
    }

    repo_id = seafile_web_access_get_repo_id (webaccess);
    data = seafile_web_access_get_obj_id (webaccess);
    operation = seafile_web_access_get_op (webaccess);
    user = seafile_web_access_get_username (webaccess);

    if (strcmp(operation, "view") != 0 &&
        strcmp(operation, "download") != 0 &&
        strcmp(operation, "download-link") != 0) {
        error = "Operation does not match access token.";
        error_code = EVHTP_RES_FORBIDDEN;
        goto on_error;
    }

    if (can_use_cached_content (req)) {
        goto success;
    }

    byte_ranges = evhtp_kv_find (req->headers_in, "Range");

    repo = seaf_repo_manager_get_repo(seaf->repo_mgr, repo_id);
    if (!repo) {
        error = "Bad repo id\n";
        goto on_error;
    }

    if (repo->encrypted) {
        key = seaf_passwd_manager_get_decrypt_key (seaf->passwd_mgr,
                                                   repo_id, user);
        if (!key) {
            error = "Repo is encrypted. Please provide password to view it.";
            goto on_error;
        }
    }

    if (!seaf_fs_manager_object_exists (seaf->fs_mgr,
                                        repo->store_id, repo->version, data)) {
        error = "Invalid file id\n";
        goto on_error;
    }

    if (!repo->encrypted && byte_ranges) {
        if (do_file_range (req, repo, data, filename, operation, byte_ranges, user) < 0) {
            error = "Internal server error\n";
            error_code = EVHTP_RES_SERVERR;
            goto on_error;
        }
    } else if (do_file(req, repo, data, filename, operation, key, user) < 0) {
        error = "Internal server error\n";
        error_code = EVHTP_RES_SERVERR;
        goto on_error;
    }

success:
    g_strfreev (parts);
    if (repo != NULL)
        seaf_repo_unref (repo);
    if (key != NULL)
        g_object_unref (key);
    if (webaccess)
        g_object_unref (webaccess);

    return;

on_error:
    g_strfreev (parts);
    if (repo != NULL)
        seaf_repo_unref (repo);
    if (key != NULL)
        g_object_unref (key);
    if (webaccess != NULL)
        g_object_unref (webaccess);

    evbuffer_add_printf(req->buffer_out, "%s\n", error);
    evhtp_send_reply(req, error_code);
}

static int
do_block(evhtp_request_t *req, SeafRepo *repo, const char *user, const char *file_id,
         const char *blk_id)
{
    Seafile *file;
    uint32_t bsize;
    gboolean found = FALSE;
    int i;
    char blk_size[255];
    char cont_filename[SEAF_PATH_MAX];
    SendBlockData *data;

    file = seaf_fs_manager_get_seafile(seaf->fs_mgr,
                                       repo->store_id, repo->version, file_id);
    if (file == NULL)
        return -1;

    for (i = 0; i < file->n_blocks; i++) {
        if (memcmp(file->blk_sha1s[i], blk_id, 40) == 0) {
            BlockMetadata *bm = seaf_block_manager_stat_block (seaf->block_mgr,
                                                               repo->store_id,
                                                               repo->version,
                                                               blk_id);
            if (bm && bm->size >= 0) {
                bsize = bm->size;
                found = TRUE;
            }
            g_free (bm);
            break;
        }
    }

    seafile_unref (file);

    /* block not found. */
    if (!found) {
        evhtp_send_reply (req, EVHTP_RES_BADREQ);
        return 0;
    }
    evhtp_headers_add_header(req->headers_out,
                             evhtp_header_new("Access-Control-Allow-Origin",
                                              "*", 1, 1));

    if (test_firefox (req)) {
        snprintf(cont_filename, SEAF_PATH_MAX,
                 "attachment;filename*=\"utf-8\' \'%s\"", blk_id);
    } else {
        snprintf(cont_filename, SEAF_PATH_MAX,
                 "attachment;filename=\"%s\"", blk_id);
    }
    evhtp_headers_add_header(req->headers_out,
                             evhtp_header_new("Content-Disposition", cont_filename,
                                              1, 1));

    snprintf(blk_size, sizeof(blk_size), "%"G_GUINT32_FORMAT"", bsize);
    evhtp_headers_add_header (req->headers_out,
                              evhtp_header_new("Content-Length", blk_size, 1, 1));

    data = g_new0 (SendBlockData, 1);
    data->req = req;
    data->block_id = g_strdup(blk_id);
    data->user = g_strdup(user);

    memcpy (data->store_id, repo->store_id, 36);
    data->repo_version = repo->version;

    /* We need to overwrite evhtp's callback functions to
     * write file data piece by piece.
     */
    struct bufferevent *bev = evhtp_request_get_bev (req);
    data->saved_read_cb = bev->readcb;
    data->saved_write_cb = bev->writecb;
    data->saved_event_cb = bev->errorcb;
    data->saved_cb_arg = bev->cbarg;
    data->bsize = bsize;
    bufferevent_setcb (bev,
                       NULL,
                       write_block_data_cb,
                       my_block_event_cb,
                       data);
    /* Block any new request from this connection before finish
     * handling this request.
     */
    evhtp_request_pause (req);

    /* Kick start data transfer by sending out http headers. */
    evhtp_send_reply_start(req, EVHTP_RES_OK);

    return 0;
}

static void
access_blks_cb(evhtp_request_t *req, void *arg)
{
    SeafRepo *repo = NULL;
    char *error = NULL;
    char *token = NULL;
    char *blkid = NULL;
    const char *repo_id = NULL;
    const char *id = NULL;
    const char *operation = NULL;
    const char *user = NULL;
    int error_code = EVHTP_RES_BADREQ;

    char *repo_role = NULL;
    SeafileWebAccess *webaccess = NULL;

    /* Skip the first '/'. */
    char **parts = g_strsplit (req->uri->path->full + 1, "/", 0);
    if (!parts || g_strv_length (parts) < 3 ||
        strcmp (parts[0], "blks") != 0) {
        error = "Invalid URL";
        goto on_error;
    }

    token = parts[1];
    blkid = parts[2];

    webaccess = seaf_web_at_manager_query_access_token (seaf->web_at_mgr, token);
    if (!webaccess) {
        error = "Access token not found";
        error_code = EVHTP_RES_FORBIDDEN;
        goto on_error;
    }

    if (can_use_cached_content (req)) {
        goto success;
    }

    repo_id = seafile_web_access_get_repo_id (webaccess);
    id = seafile_web_access_get_obj_id (webaccess);
    operation = seafile_web_access_get_op (webaccess);
    user = seafile_web_access_get_username (webaccess);

    repo = seaf_repo_manager_get_repo(seaf->repo_mgr, repo_id);
    if (!repo) {
        error = "Bad repo id\n";
        goto on_error;
    }

    if (!seaf_fs_manager_object_exists (seaf->fs_mgr,
                                        repo->store_id, repo->version, id)) {
        error = "Invalid file id\n";
        goto on_error;
    }

    if (strcmp(operation, "downloadblks") == 0) {
        if (do_block(req, repo, user, id, blkid) < 0) {
            seaf_warning ("Failed to download blocks for token: %s\n", token);
            error_code = EVHTP_RES_SERVERR;
            goto on_error;
        }
    }

success:
    g_strfreev (parts);
    if (repo != NULL)
        seaf_repo_unref (repo);
    g_free (repo_role);
    g_object_unref (webaccess);

    return;

on_error:
    g_strfreev (parts);
    if (repo != NULL)
        seaf_repo_unref (repo);
    g_free (repo_role);
    if (webaccess != NULL)
        g_object_unref (webaccess);

    evbuffer_add_printf(req->buffer_out, "%s\n", error);
    evhtp_send_reply(req, error_code);
}

int
access_file_init (evhtp_t *htp)
{
    evhtp_set_regex_cb (htp, "^/files/.*", access_cb, NULL);
    evhtp_set_regex_cb (htp, "^/blks/.*", access_blks_cb, NULL);
    evhtp_set_regex_cb (htp, "^/zip/.*", access_zip_cb, NULL);

    return 0;
}

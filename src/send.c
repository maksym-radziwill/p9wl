#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <unistd.h>

#include <wlr/util/log.h>
#include <wlr/types/wlr_output.h>

#include <freerdp/freerdp.h>
#include <freerdp/listener.h>
#include <freerdp/peer.h>
#include <freerdp/settings.h>
#include <freerdp/crypto/certificate.h>
#include <freerdp/crypto/privatekey.h>
#include <winpr/wtsapi.h>
#include <freerdp/primary.h>
#include <freerdp/codec/rfx.h>
#include "types.h"

#define TAG FREERDP_TAG("p9wl.send")

struct rdp_send_ctx {
    freerdp *instance;
    rdpContext *context;
    freerdp_peer *peer;
    RFX_CONTEXT *rfx_context;
    wStream *encode_stream;
    uint32_t frame_id;
    atomic_int pending;
    atomic_int errors;
    atomic_int running;
    atomic_int paused;
    atomic_int broken;
    pthread_t thread;
    pthread_mutex_t lock;
    pthread_cond_t cond;
    pthread_cond_t done_cond;
    int width;
    int height;
    struct server *server;
};

static struct rdp_send_ctx rdp_ctx;

// Forward declaration of send_frame so it can be called from post_connect
void send_frame(struct server *s);

static void dump_framebuffer_ppm(const char *filename, uint32_t *pixels, int width, int height) {
    FILE *f = fopen(filename, "wb");
    if (!f) {
        wlr_log(WLR_ERROR, "Failed to open %s for framebuffer dump: %s", filename, strerror(errno));
        return;
    }
    fprintf(f, "P6\n%d %d\n255\n", width, height);
    for (int i = 0; i < width * height; i++) {
        uint32_t p = pixels[i];
        uint8_t b = (p >> 0) & 0xFF;
        uint8_t g = (p >> 8) & 0xFF;
        uint8_t r = (p >> 16) & 0xFF;
        fputc(r, f);
        fputc(g, f);
        fputc(b, f);
    }
    fclose(f);
    wlr_log(WLR_INFO, "Successfully dumped framebuffer to %s (%dx%d)", filename, width, height);
}

static inline void rdp_drain_wake(void) {
    pthread_mutex_lock(&rdp_ctx.lock);
    pthread_cond_signal(&rdp_ctx.cond);
    pthread_mutex_unlock(&rdp_ctx.lock);
}

static void *rdp_drain_thread_func(void *arg) {
    (void)arg;
    wlr_log(WLR_INFO, "RDP Channel Drain / Event thread started");
    
    while (atomic_load(&rdp_ctx.running)) {
        freerdp_peer *peer = rdp_ctx.peer;
        if (!peer) {
            usleep(10000);
            if (atomic_load(&rdp_ctx.broken)) break;
            continue;
        }

        HANDLE handles[64];
        DWORD handle_count = 0;
        if (peer->GetEventHandles) {
            handle_count = peer->GetEventHandles(peer, handles, 64);
        }

        if (handle_count > 0) {
            DWORD status = WaitForMultipleObjects(handle_count, handles, FALSE, 50);
            if (status == WAIT_FAILED) {
                atomic_store(&rdp_ctx.broken, 1);
                break;
            }
        } else {
            usleep(50000);
        }

        if (peer->CheckFileDescriptor) {
            if (peer->CheckFileDescriptor(peer) != TRUE) {
                wlr_log(WLR_INFO, "RDP peer disconnected or check file descriptor failed");
                atomic_store(&rdp_ctx.broken, 1);
                break;
            }
        }

        if (freerdp_shall_disconnect_context(rdp_ctx.context)) {
            atomic_store(&rdp_ctx.broken, 1);
            break;
        }
        
        if (atomic_load(&rdp_ctx.pending) > 0) {
            atomic_fetch_sub(&rdp_ctx.pending, 1);
            pthread_mutex_lock(&rdp_ctx.lock);
            pthread_cond_broadcast(&rdp_ctx.done_cond);
            pthread_mutex_unlock(&rdp_ctx.lock);
        }
    }
    
    wlr_log(WLR_INFO, "RDP Channel Drain thread exiting");
    return NULL;
}

static int rdp_drain_start(freerdp *instance, rdpContext *context, freerdp_peer *peer, int width, int height) {
    atomic_store(&rdp_ctx.pending, 0);
    atomic_store(&rdp_ctx.errors, 0);
    atomic_store(&rdp_ctx.running, 1);
    atomic_store(&rdp_ctx.paused, 0);
    atomic_store(&rdp_ctx.broken, 0);
    rdp_ctx.instance = instance;
    rdp_ctx.context = context;
    rdp_ctx.peer = peer;
    rdp_ctx.width = width;
    rdp_ctx.height = height;
    rdp_ctx.frame_id = 0;
    
    pthread_mutex_init(&rdp_ctx.lock, NULL);
    pthread_cond_init(&rdp_ctx.cond, NULL);
    pthread_cond_init(&rdp_ctx.done_cond, NULL);
    
    rdp_ctx.rfx_context = rfx_context_new(TRUE);
    if (!rdp_ctx.rfx_context) return -1;
    rfx_context_set_pixel_format(rdp_ctx.rfx_context, PIXEL_FORMAT_BGRX32);
    rfx_context_set_mode(rdp_ctx.rfx_context, RLGR3);
    if (!rfx_context_reset(rdp_ctx.rfx_context, width, height)) {
        rfx_context_free(rdp_ctx.rfx_context);
        rdp_ctx.rfx_context = NULL;
        return -1;
    }

    rdp_ctx.encode_stream = Stream_New(NULL, 65536);
    if (!rdp_ctx.encode_stream) {
        rfx_context_free(rdp_ctx.rfx_context);
        rdp_ctx.rfx_context = NULL;
        return -1;
    }

    if (pthread_create(&rdp_ctx.thread, NULL, rdp_drain_thread_func, NULL) != 0) {
        Stream_Free(rdp_ctx.encode_stream, TRUE);
        rdp_ctx.encode_stream = NULL;
        rfx_context_free(rdp_ctx.rfx_context);
        rdp_ctx.rfx_context = NULL;
        return -1;
    }
    return 0;
}

static void rdp_drain_stop(void) {
    atomic_store(&rdp_ctx.running, 0);
    rdp_drain_wake();
    pthread_join(rdp_ctx.thread, NULL);
    
    if (rdp_ctx.encode_stream) {
        Stream_Free(rdp_ctx.encode_stream, TRUE);
        rdp_ctx.encode_stream = NULL;
    }
    
    if (rdp_ctx.rfx_context) {
        rfx_context_free(rdp_ctx.rfx_context);
        rdp_ctx.rfx_context = NULL;
    }
    
    pthread_mutex_destroy(&rdp_ctx.lock);
    pthread_cond_destroy(&rdp_ctx.cond);
    pthread_cond_destroy(&rdp_ctx.done_cond);
}

static void rdp_drain_pause(void) {
    atomic_store(&rdp_ctx.paused, 1);
    rdp_drain_wake();
    pthread_mutex_lock(&rdp_ctx.lock);
    while (atomic_load(&rdp_ctx.pending) > 0 && !atomic_load(&rdp_ctx.broken)) {
        pthread_cond_wait(&rdp_ctx.done_cond, &rdp_ctx.lock);
    }
    pthread_mutex_unlock(&rdp_ctx.lock);
}

static void rdp_drain_resume(void) {
    atomic_store(&rdp_ctx.paused, 0);
    rdp_drain_wake();
}

static void rdp_drain_notify(void) {
    if (atomic_load(&rdp_ctx.broken)) return;
    atomic_fetch_add(&rdp_ctx.pending, 1);
    rdp_drain_wake();
}

static void rdp_drain_throttle(int max_pending) {
    pthread_mutex_lock(&rdp_ctx.lock);
    while (atomic_load(&rdp_ctx.pending) > max_pending && !atomic_load(&rdp_ctx.broken)) {
        pthread_cond_wait(&rdp_ctx.done_cond, &rdp_ctx.lock);
    }
    pthread_mutex_unlock(&rdp_ctx.lock);
}

static BOOL p9wl_peer_context_new(freerdp_peer *peer, rdpContext *context) {
    (void)peer;
    (void)context;
    return TRUE;
}

static void p9wl_peer_context_free(freerdp_peer *peer, rdpContext *context) {
    (void)peer;
    (void)context;
}

static BOOL p9wl_peer_mouse_event(rdpInput *input, UINT16 flags, UINT16 x, UINT16 y) {
    (void)input;
    if (rdp_ctx.server) {
        rdp_handle_mouse(rdp_ctx.server, flags, x, y);
    }
    return TRUE;
}

static BOOL p9wl_peer_keyboard_event(rdpInput *input, UINT16 flags, UINT8 code) {
    (void)input;
    if (rdp_ctx.server) {
        rdp_handle_keyboard(rdp_ctx.server, flags, code);
    }
    return TRUE;
}

static BOOL p9wl_peer_post_connect(freerdp_peer *peer) {
    wlr_log(WLR_INFO, "RDP peer post-connect handshake complete");    
    rdpSettings *settings = peer->context->settings;
    if (settings) {
        freerdp_settings_set_uint32(settings, FreeRDP_ColorDepth, 32);
    }

    if (rdp_ctx.server) {
        pthread_mutex_lock(&rdp_ctx.server->send_lock);
        rdp_ctx.server->force_full_frame = 1;
        rdp_ctx.server->frame_dirty = 1;
        pthread_mutex_unlock(&rdp_ctx.server->send_lock);

        send_frame(rdp_ctx.server);

        if (rdp_ctx.server->output) {
            wlr_output_schedule_frame(rdp_ctx.server->output);
        }
    }

    return TRUE;
}

static BOOL rdp_peer_accepted_callback(freerdp_listener *listener, freerdp_peer *peer) {
    (void)listener;

    peer->ContextNew = p9wl_peer_context_new;
    peer->ContextFree = p9wl_peer_context_free;

    if (!freerdp_peer_context_new(peer)) {
        wlr_log(WLR_ERROR, "Failed to allocate RDP peer context");
        return FALSE;
    }
    
    int w = rdp_ctx.server ? rdp_ctx.server->width : 1024;
    int h = rdp_ctx.server ? rdp_ctx.server->height : 768;

    rdpSettings *settings = peer->context->settings;
    if (settings) {
        freerdp_settings_set_bool(settings, FreeRDP_NlaSecurity, FALSE);
        freerdp_settings_set_bool(settings, FreeRDP_TlsSecurity, TRUE);
        freerdp_settings_set_bool(settings, FreeRDP_RdpSecurity, FALSE);
        freerdp_settings_set_bool(settings, FreeRDP_UseRdpSecurityLayer, FALSE);

        freerdp_settings_set_bool(settings, FreeRDP_RemoteFxCodec, TRUE);
        freerdp_settings_set_bool(settings, FreeRDP_NSCodec, TRUE);
        freerdp_settings_set_bool(settings, FreeRDP_SuppressOutput, FALSE);
        freerdp_settings_set_uint32(settings, FreeRDP_ColorDepth, 32);
        freerdp_settings_set_uint32(settings, FreeRDP_DesktopWidth, w);
        freerdp_settings_set_uint32(settings, FreeRDP_DesktopHeight, h);

        rdpCertificate *cert = freerdp_certificate_new_from_file("/app/server.crt");
        if (cert) {
            freerdp_settings_set_pointer_len(settings, FreeRDP_RdpServerCertificate, cert, 1);
        } else {
            wlr_log(WLR_ERROR, "Failed to load RDP server certificate from /app/server.crt");
        }

        rdpPrivateKey *key = freerdp_key_new_from_file_enc("/app/server.key", NULL);
        if (key) {
            freerdp_settings_set_pointer_len(settings, FreeRDP_RdpServerRsaKey, key, 1);
        } else {
            wlr_log(WLR_ERROR, "Failed to load RDP private key from /app/server.key");
        }
    }

    peer->PostConnect = p9wl_peer_post_connect;
    if (peer->context && peer->context->input) {
        peer->context->input->MouseEvent = p9wl_peer_mouse_event;
        peer->context->input->KeyboardEvent = p9wl_peer_keyboard_event;
    }

    rdp_ctx.instance = peer->context->instance;
    rdp_ctx.context = peer->context;
    rdp_ctx.peer = peer;

    if (rdp_drain_start(rdp_ctx.instance, rdp_ctx.context, peer, w, h) < 0) {
        wlr_log(WLR_ERROR, "Failed to start RDP drain thread during peer acceptance");
        freerdp_peer_context_free(peer);
        return FALSE;
    }

    if (!peer->Initialize(peer)) {
        wlr_log(WLR_ERROR, "Failed to initialize RDP peer");
        rdp_drain_stop();
        freerdp_peer_context_free(peer);
        return FALSE;
    }
    
    wlr_log(WLR_INFO, "RDP client socket accepted, initializing handshake...");
    return TRUE;
}

void send_frame(struct server *s) {
    pthread_mutex_lock(&s->send_lock);
    
    if (s->resize_pending) {
        pthread_mutex_unlock(&s->send_lock);
        return;
    }

    int buf = -1;
    for (int i = 0; i < 2; i++) {
        if (i != s->active_buf && i != s->pending_buf) {
            buf = i;
            break;
        }
    }
    
    if (buf < 0) {
        pthread_mutex_unlock(&s->send_lock);
        return;
    }
    
    uint32_t *tmp     = s->send_buf[buf];
    s->send_buf[buf]  = s->framebuf;
    s->framebuf       = tmp;

    if (s->dirty_staging_valid) {
        int ntiles = s->tiles_x * s->tiles_y;
        if (!s->dirty_tiles[buf] && ntiles > 0)
            s->dirty_tiles[buf] = calloc(1, ntiles);
        if (s->dirty_tiles[buf] && ntiles > 0) {
            memcpy(s->dirty_tiles[buf], s->dirty_staging, ntiles);
            s->dirty_valid[buf] = 1;
        } else {
            s->dirty_valid[buf] = 0;
        }
        s->dirty_staging_valid = 0;
    } else {
        s->dirty_valid[buf] = 0;
    }

    s->pending_buf = buf;
    if (s->force_full_frame) s->send_full = 1;
    pthread_cond_signal(&s->send_cond);
    pthread_mutex_unlock(&s->send_lock);
}

int send_timer_callback(void *data) {
    struct server *s = data;
    if (!s->frame_dirty) return 0;
    s->frame_dirty = 0;
    send_frame(s);
    return 0;
}

static int scroll_disabled(struct server *s) {
    double floor_val;
    return (modf(s->scale, &floor_val) != 0.0);
}

void *send_thread_func(void *arg) {
    struct server *s = arg;
    
    wlr_log(WLR_INFO, "RDP Send thread started");
    rdp_ctx.server = s;
    
    freerdp_listener *listener = freerdp_listener_new();
    if (!listener) {
        wlr_log(WLR_ERROR, "Failed to create FreeRDP listener");
        return NULL;
    }

    listener->PeerAccepted = rdp_peer_accepted_callback;

    if (!listener->Open(listener, NULL, 3389)) {
        wlr_log(WLR_ERROR, "Failed to bind FreeRDP listener to port 3389");
        freerdp_listener_free(listener);
        return NULL;
    }

    wlr_log(WLR_INFO, "Waiting for RDP client connection on port 3389...");

    while (s->running && !rdp_ctx.instance) {
        HANDLE handles[64];
        DWORD count = listener->GetEventHandles(listener, handles, 64);
        if (count == 0) break;
        
        DWORD status = WaitForMultipleObjects(count, handles, FALSE, 100);
        if (status == WAIT_FAILED) break;
        
        if (listener->CheckFileDescriptor(listener) != TRUE) {
            break;
        }
    }

    listener->Close(listener);
    freerdp_listener_free(listener);

    if (!s->running || !rdp_ctx.instance) {
        return NULL;
    }

    if (scroll_disabled(s)) {
        wlr_log(WLR_INFO, "Scroll optimization disabled (fractional scale: %.2f)", s->scale);
    }
    
    int max_tiles = (4096 / TILE_SIZE) * (4096 / TILE_SIZE);
    struct tile_work *work = malloc(max_tiles * sizeof(*work));
    struct tile_result *results = malloc(max_tiles * sizeof(*results));
    
    int draw_suspended = 0;
    if (!work || !results) {
        rdp_drain_stop();
        free(work);
        free(results);
        return NULL;
    }
    
    int initial_dump_done = 0;

    while (s->running) {
        pthread_mutex_lock(&s->send_lock);
        while (s->pending_buf < 0 && !s->window_changed && s->running) {
            pthread_cond_wait(&s->send_cond, &s->send_lock);
        }
        pthread_mutex_unlock(&s->send_lock);
        if (!s->running) break;

        pthread_mutex_lock(&s->send_lock);
        int current_buf = s->pending_buf;
        int got_frame = (current_buf >= 0);
        if (got_frame) {
            s->active_buf = current_buf;
            s->pending_buf = -1;
        }
        int do_full = s->send_full;
        s->send_full = 0;
        pthread_mutex_unlock(&s->send_lock);
        
        uint32_t *send_buf = got_frame ? s->send_buf[current_buf] : NULL;
        
        if (send_buf && !initial_dump_done) {
            dump_framebuffer_ppm("/app/compositor_frame.ppm", send_buf, s->width, s->height);
            initial_dump_done = 1;
        }

        if (send_buf &&
            (s->visible_width < s->width || s->visible_height < s->height)) {
            if (s->visible_width < s->width) {
                int pad = s->width - s->visible_width;
                for (int y = 0; y < s->visible_height; y++)
                    memset(&send_buf[y * s->width + s->visible_width], 0,
                           pad * sizeof(uint32_t));
            }
            if (s->visible_height < s->height) {
                memset(&send_buf[s->visible_height * s->width], 0,
                       (s->height - s->visible_height) * s->width
                       * sizeof(uint32_t));
            }
        }
        
        if (atomic_load(&rdp_ctx.broken)) {
            if (!draw_suspended) {
                draw_suspended = 1;
                s->running = 0;
                struct input_event wakeup = { .type = INPUT_WAKEUP };
                input_queue_push(&s->input_queue, &wakeup);
            }
            s->window_changed = 0;
            if (got_frame) {
                pthread_mutex_lock(&s->send_lock);
                s->active_buf = -1;
                pthread_mutex_unlock(&s->send_lock);
            }
            break;
        }
        
        if (s->window_changed) {
            s->window_changed = 0;
            rdp_drain_pause();
            if (relookup_window(s) == 0) {
                draw_suspended = 0;
            } else {
                draw_suspended = 1;
            }
            rdp_drain_resume();
            struct input_event wakeup = { .type = INPUT_WAKEUP };
            input_queue_push(&s->input_queue, &wakeup);
            if (s->resize_pending) {
                pthread_mutex_lock(&s->send_lock);
                s->active_buf = -1;
                pthread_mutex_unlock(&s->send_lock);
                continue;
            }
            do_full = 1;
        }
        
        if (!got_frame) continue;
        if (s->resize_pending || draw_suspended) {
            pthread_mutex_lock(&s->send_lock);
            s->active_buf = -1;
            pthread_mutex_unlock(&s->send_lock);
            continue;
        }
        if (s->force_full_frame) {
            do_full = 1;
            s->force_full_frame = 0;
        }
        
        if (rdp_ctx.rfx_context && (rdp_ctx.width != s->width || rdp_ctx.height != s->height)) {
            rfx_context_reset(rdp_ctx.rfx_context, s->width, s->height);
            rdp_ctx.width = s->width;
            rdp_ctx.height = s->height;
        }

        int scrolled_regions = 0;
        if (!do_full && !scroll_disabled(s)) {
            detect_scroll(s, send_buf);
            scrolled_regions = apply_scroll_to_prevbuf(s);
        }
        
        uint8_t *dirty_map = NULL;
        if (!do_full && scrolled_regions == 0 &&
            s->dirty_valid[current_buf] && s->dirty_tiles[current_buf]) {
            dirty_map = s->dirty_tiles[current_buf];
        }
        
        int work_count = 0;
        for (int ty = 0; ty < s->tiles_y; ty++) {
            for (int tx = 0; tx < s->tiles_x; tx++) {
                int x1, y1, w, h;
                tile_bounds(tx, ty, s->width, s->height, &x1, &y1, &w, &h);
                if (w <= 0 || h <= 0) continue;
                
                int changed;
                if (dirty_map) {
                    if (!dirty_map[ty * s->tiles_x + tx]) continue;
                    changed = tile_changed(send_buf, s->prev_framebuf,
                                           s->width, x1, y1, w, h);
                } else {
                    changed = do_full || tile_changed(send_buf, s->prev_framebuf,
                                                       s->width, x1, y1, w, h);
                }
                if (!changed) continue;
                if (work_count >= max_tiles) break;
                
                work[work_count] = (struct tile_work){
                    .pixels = send_buf, .stride = s->width,
                    .prev_pixels = NULL, .prev_stride = s->width,
                    .x1 = x1, .y1 = y1, .w = w, .h = h
                };
                work_count++;
            }
        }
        
        rdp_drain_throttle(2);
        
        if (work_count > 0 && rdp_ctx.context && rdp_ctx.context->update) {
            rdpUpdate *update = rdp_ctx.context->update;
            if (update->SurfaceFrameMarker) {
                SURFACE_FRAME_MARKER fm = { 0 };
                fm.frameAction = SURFACECMD_FRAMEACTION_BEGIN;
                fm.frameId = rdp_ctx.frame_id;
                update->SurfaceFrameMarker(update->context, &fm);
            }

            for (int i = 0; i < work_count; i++) {
                struct tile_work *tw = &work[i];
                int x1 = tw->x1, y1 = tw->y1;
                int w = tw->w, h = tw->h;
                
                if (rdp_ctx.rfx_context && rdp_ctx.encode_stream) {
                    SURFACE_BITS_COMMAND cmd = { 0 };
                    RFX_RECT rfx_rect = { .x = 0, .y = 0, .width = w, .height = h };
                    
                    Stream_SetPosition(rdp_ctx.encode_stream, 0);
                    uint32_t *tile_pixels = &send_buf[y1 * s->width + x1];
                    
                    rfx_context_set_pixel_format(rdp_ctx.rfx_context, PIXEL_FORMAT_BGRX32);
                    if (rfx_compose_message(rdp_ctx.rfx_context, rdp_ctx.encode_stream, &rfx_rect, 1,
                                            (BYTE *)tile_pixels, w, h, s->width * sizeof(uint32_t))) {
                        cmd.destLeft = x1;
                        cmd.destTop = y1;
                        cmd.destRight = x1 + w;
                        cmd.destBottom = y1 + h;
                        cmd.bmp.bpp = 32;
                        cmd.bmp.width = w;
                        cmd.bmp.height = h;
                        cmd.bmp.bitmapDataLength = Stream_GetPosition(rdp_ctx.encode_stream);
                        cmd.bmp.bitmapData = Stream_Buffer(rdp_ctx.encode_stream);
                        
                        if (update && update->SurfaceBits) {
                            update->SurfaceBits(update->context, &cmd);
                        }
                    }
                    rdp_drain_notify();
                }
                
                for (int row = 0; row < h; row++) {
                    memcpy(&s->prev_framebuf[(y1 + row) * s->width + x1],
                           &send_buf[(y1 + row) * s->width + x1], w * 4);
                }
            }

            if (update->SurfaceFrameMarker) {
                SURFACE_FRAME_MARKER fm = { 0 };
                fm.frameAction = SURFACECMD_FRAMEACTION_END;
                fm.frameId = rdp_ctx.frame_id++;
                update->SurfaceFrameMarker(update->context, &fm);
            }
        }
        
        pthread_mutex_lock(&s->send_lock);
        s->active_buf = -1;
        pthread_mutex_unlock(&s->send_lock);
    }
    
    rdp_drain_stop();
    free(work);
    free(results);
    wlr_log(WLR_INFO, "RDP Send thread exiting");
    return NULL;
}
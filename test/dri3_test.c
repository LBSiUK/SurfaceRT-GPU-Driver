/*
 * Tiny XCB DRI3 round-trip test for the opentegra DRI3 skeleton.
 *
 *   1. Connect to X server, query DRI3 extension.
 *   2. CreatePixmap (1024x768 ARGB) — large enough to force BO not POOL.
 *   3. xcb_dri3_buffers_from_pixmap_reply  → exercises fds_from_pixmap.
 *   4. xcb_dri3_pixmap_from_buffers       → exercises pixmap_from_fds,
 *                                           using the fd from step 3.
 *   5. Free both pixmaps; close.
 *
 * Each protocol step prints what it did so we can correlate with the
 * "DRI3: first ..." once-only log lines in /var/log/Xorg.0.log.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <xcb/xcb.h>
#include <xcb/dri3.h>
#include <xcb/present.h>
#include <xcb/sync.h>

#define W 1024
#define H 768

int main(void)
{
    int screen_num;
    xcb_connection_t *conn = xcb_connect(NULL, &screen_num);
    if (!conn || xcb_connection_has_error(conn)) {
        fprintf(stderr, "xcb_connect failed\n");
        return 1;
    }

    const xcb_setup_t *setup = xcb_get_setup(conn);
    xcb_screen_iterator_t it = xcb_setup_roots_iterator(setup);
    for (int i = 0; i < screen_num; i++) xcb_screen_next(&it);
    xcb_screen_t *screen = it.data;

    /* Confirm DRI3 is present. */
    xcb_query_extension_cookie_t qec =
        xcb_query_extension(conn, 4, "DRI3");
    xcb_query_extension_reply_t *qer =
        xcb_query_extension_reply(conn, qec, NULL);
    if (!qer || !qer->present) {
        fprintf(stderr, "DRI3 extension not present\n");
        return 2;
    }
    printf("DRI3 extension: opcode=%u event=%u error=%u\n",
           qer->major_opcode, qer->first_event, qer->first_error);
    free(qer);

    /* Create a pixmap big enough to land in a BO. */
    xcb_pixmap_t pix = xcb_generate_id(conn);
    xcb_create_pixmap(conn, 32, pix, screen->root, W, H);
    xcb_flush(conn);
    printf("created pixmap id=0x%x  %dx%d depth=32\n", pix, W, H);

    /* Export it via DRI3 -> exercises fds_from_pixmap. */
    xcb_dri3_buffers_from_pixmap_cookie_t bc =
        xcb_dri3_buffers_from_pixmap(conn, pix);
    xcb_generic_error_t *err = NULL;
    xcb_dri3_buffers_from_pixmap_reply_t *br =
        xcb_dri3_buffers_from_pixmap_reply(conn, bc, &err);
    if (!br || err) {
        fprintf(stderr, "buffers_from_pixmap failed (err=%d)\n",
                err ? err->error_code : -1);
        return 3;
    }
    int nfds = br->nfd;
    int *fds = xcb_dri3_buffers_from_pixmap_reply_fds(conn, br);
    uint32_t *strides = xcb_dri3_buffers_from_pixmap_strides(br);
    uint32_t *offsets = xcb_dri3_buffers_from_pixmap_offsets(br);
    printf("export ok: nfds=%d stride0=%u offset0=%u modifier=0x%llx "
           "depth=%u bpp=%u %ux%u\n",
           nfds, strides[0], offsets[0],
           (unsigned long long)br->modifier,
           br->depth, br->bpp, br->width, br->height);
    if (nfds < 1) return 4;

    /* Import the dma-buf back as a new pixmap -> exercises pixmap_from_fds. */
    xcb_pixmap_t pix2 = xcb_generate_id(conn);
    int dup_fd = dup(fds[0]);   /* xcb consumes the fd; dup so we keep one */
    if (dup_fd < 0) { perror("dup"); return 5; }
    xcb_dri3_pixmap_from_buffers(conn, pix2, screen->root,
                                 nfds, br->width, br->height,
                                 strides[0], offsets[0],
                                 strides[1], offsets[1],
                                 strides[2], offsets[2],
                                 strides[3], offsets[3],
                                 br->depth, br->bpp, br->modifier,
                                 &dup_fd);
    /* xcb_dri3_pixmap_from_buffers is a void request; round-trip via
     * GetGeometry on the new pixmap to flush and detect a server-side
     * protocol error. */
    xcb_get_geometry_cookie_t gc = xcb_get_geometry(conn, pix2);
    xcb_get_geometry_reply_t *gr =
        xcb_get_geometry_reply(conn, gc, &err);
    if (!gr || err) {
        fprintf(stderr, "pixmap_from_buffers protocol error (err=%d)\n",
                err ? err->error_code : -1);
        return 6;
    }
    printf("import ok: pixmap 0x%x reported %ux%u depth=%u\n",
           pix2, gr->width, gr->height, gr->depth);
    free(gr);

    /* ---- Present test: ask for a vblank notify 2 frames out. ---- */
    xcb_query_extension_cookie_t pqc =
        xcb_query_extension(conn, 7, "Present");
    xcb_query_extension_reply_t *pqr =
        xcb_query_extension_reply(conn, pqc, NULL);
    if (!pqr || !pqr->present) {
        fprintf(stderr, "Present extension not present\n");
        return 7;
    }
    printf("Present extension: opcode=%u event=%u error=%u\n",
           pqr->major_opcode, pqr->first_event, pqr->first_error);
    free(pqr);

    /* Get current MSC via PresentQueryVersion + a PresentNotifyMSC. We
     * use the root window since it always has a CRTC. */
    xcb_window_t root = screen->root;
    uint32_t serial = 0xdeadbeef;
    xcb_present_select_input(conn, xcb_generate_id(conn), root,
                             XCB_PRESENT_EVENT_MASK_COMPLETE_NOTIFY);
    xcb_present_notify_msc(conn, root, serial,
                           /* target_msc */ 0,  /* 0 = next vblank */
                           /* divisor    */ 0,
                           /* remainder  */ 0);
    xcb_flush(conn);

    /* Wait for the CompleteNotify event (or any error). */
    int got_present = 0;
    for (int tries = 0; tries < 20 && !got_present; tries++) {
        xcb_generic_event_t *ev = xcb_wait_for_event(conn);
        if (!ev) break;
        if ((ev->response_type & 0x7f) == XCB_GE_GENERIC) {
            xcb_ge_generic_event_t *ge = (void *)ev;
            if (ge->extension == 149 /* dri3 */) { /* ignore */ }
            /* Present events come through GE_GENERIC with the Present
             * extension's opcode; the inner event_type 1 is CompleteNotify. */
            if (ge->event_type == XCB_PRESENT_COMPLETE_NOTIFY) {
                xcb_present_complete_notify_event_t *pe = (void *)ev;
                printf("present complete: kind=%u mode=%u serial=0x%x "
                       "ust=%llu msc=%llu\n",
                       pe->kind, pe->mode, pe->serial,
                       (unsigned long long)pe->ust,
                       (unsigned long long)pe->msc);
                got_present = 1;
            }
        }
        free(ev);
    }
    if (!got_present) {
        fprintf(stderr, "Present notify_msc: no CompleteNotify received\n");
        return 8;
    }

    /* Cleanup. */
    xcb_free_pixmap(conn, pix2);
    xcb_free_pixmap(conn, pix);
    free(br);
    xcb_flush(conn);
    xcb_disconnect(conn);
    printf("OK\n");
    return 0;
}

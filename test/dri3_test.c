/*
 * XCB DRI3 + Present round-trip test for the opentegra driver.
 *
 *   1. Connect to X server, query DRI3.
 *   2. CreatePixmap (1024x768 ARGB) — large enough to force BO not POOL.
 *   3. xcb_dri3_buffers_from_pixmap_reply  → exercises fds_from_pixmap.
 *   4. xcb_dri3_pixmap_from_buffers        → exercises pixmap_from_fds.
 *   5. Present notify_msc on the root          → exercises queue_vblank.
 *   6. Present a screen-sized pixmap to a fullscreen override-redirect
 *      window and report whether the CompleteNotify came back as FLIP
 *      or COPY → exercises check_flip / flip.
 *
 * Each step prints what it did so it can be correlated with the
 * once-only "DRI3: ..." / "Present: ..." lines in /var/log/Xorg.0.log.
 *
 * Exit status: 0 once every protocol step succeeds. Whether step 6
 * page-flips or copies is reported but does not change the status — a
 * running compositor or a pitch mismatch legitimately forces copy.
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

/* Wait for a Present CompleteNotify; returns its mode (COPY/FLIP/SKIP),
 * or -1 on timeout. Non-matching events are drained and ignored. */
static int
wait_present_complete(xcb_connection_t *conn, uint32_t want_serial,
                      uint8_t *kind_out, uint64_t *msc_out)
{
    for (int tries = 0; tries < 40; tries++) {
        xcb_generic_event_t *ev = xcb_wait_for_event(conn);
        if (!ev)
            return -1;

        int mode = -1;
        if ((ev->response_type & 0x7f) == XCB_GE_GENERIC) {
            xcb_ge_generic_event_t *ge = (void *)ev;
            if (ge->event_type == XCB_PRESENT_COMPLETE_NOTIFY) {
                xcb_present_complete_notify_event_t *pe = (void *)ev;
                if (want_serial == 0 || pe->serial == want_serial) {
                    if (kind_out) *kind_out = pe->kind;
                    if (msc_out)  *msc_out  = pe->msc;
                    mode = pe->mode;
                }
            }
        }
        free(ev);
        if (mode >= 0)
            return mode;
    }
    return -1;
}

static const char *
present_mode_str(int mode)
{
    switch (mode) {
    case XCB_PRESENT_COMPLETE_MODE_COPY:            return "COPY";
    case XCB_PRESENT_COMPLETE_MODE_FLIP:            return "FLIP";
    case XCB_PRESENT_COMPLETE_MODE_SKIP:            return "SKIP";
    case XCB_PRESENT_COMPLETE_MODE_SUBOPTIMAL_COPY: return "SUBOPTIMAL_COPY";
    default:                                        return "?";
    }
}

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

    /* ---- Present test: ask for a vblank notify on the root. ---- */
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

    xcb_window_t root = screen->root;
    uint32_t serial = 0xdeadbeef;
    xcb_present_select_input(conn, xcb_generate_id(conn), root,
                             XCB_PRESENT_EVENT_MASK_COMPLETE_NOTIFY);
    xcb_present_notify_msc(conn, root, serial,
                           /* target_msc */ 0,  /* 0 = next vblank */
                           /* divisor    */ 0,
                           /* remainder  */ 0);
    xcb_flush(conn);

    {
        uint8_t kind = 0;
        uint64_t msc = 0;
        int mode = wait_present_complete(conn, serial, &kind, &msc);
        if (mode < 0) {
            fprintf(stderr, "Present notify_msc: no CompleteNotify\n");
            return 8;
        }
        printf("present complete: kind=%u mode=%u (%s) msc=%llu\n",
               kind, mode, present_mode_str(mode),
               (unsigned long long)msc);
    }

    /* ---- Page-flip test: present a screen-sized pixmap to a
     * fullscreen override-redirect window. check_flip should accept it
     * and the CompleteNotify mode should be FLIP. ---- */
    uint16_t sw = screen->width_in_pixels;
    uint16_t sh = screen->height_in_pixels;
    uint8_t  sd = screen->root_depth;

    xcb_window_t win = xcb_generate_id(conn);
    uint32_t win_vals[2] = {
        1,                              /* XCB_CW_OVERRIDE_REDIRECT */
        XCB_EVENT_MASK_STRUCTURE_NOTIFY /* XCB_CW_EVENT_MASK */
    };
    xcb_create_window(conn, sd, win, screen->root,
                      0, 0, sw, sh, 0,
                      XCB_WINDOW_CLASS_INPUT_OUTPUT, screen->root_visual,
                      XCB_CW_OVERRIDE_REDIRECT | XCB_CW_EVENT_MASK, win_vals);
    xcb_map_window(conn, win);
    uint32_t raise = XCB_STACK_MODE_ABOVE;
    xcb_configure_window(conn, win, XCB_CONFIG_WINDOW_STACK_MODE, &raise);
    xcb_flush(conn);
    printf("flip test: fullscreen override-redirect window 0x%x %ux%u depth=%u\n",
           win, sw, sh, sd);

    /* Drain events until the window is mapped (viewable) before present. */
    for (int tries = 0; tries < 20; tries++) {
        xcb_generic_event_t *ev = xcb_wait_for_event(conn);
        if (!ev) break;
        int mapped = ((ev->response_type & 0x7f) == XCB_MAP_NOTIFY);
        free(ev);
        if (mapped) break;
    }

    xcb_present_select_input(conn, xcb_generate_id(conn), win,
                             XCB_PRESENT_EVENT_MASK_COMPLETE_NOTIFY);

    /* Two screen-sized pixmaps, alternated so each present targets an
     * idle buffer. Created at the root depth so check_flip accepts them. */
    xcb_pixmap_t fpix[2];
    for (int i = 0; i < 2; i++) {
        fpix[i] = xcb_generate_id(conn);
        xcb_create_pixmap(conn, sd, fpix[i], win, sw, sh);
    }
    xcb_flush(conn);

    int got_flip = 0, got_any = 0;
    for (int f = 0; f < 8; f++) {
        uint32_t fserial = 0x5000 + f;
        xcb_present_pixmap(conn, win, fpix[f & 1], fserial,
                           XCB_NONE,  /* valid region   */
                           XCB_NONE,  /* update region  */
                           0, 0,      /* x_off, y_off    */
                           XCB_NONE,  /* target_crtc     */
                           XCB_NONE,  /* wait_fence      */
                           XCB_NONE,  /* idle_fence      */
                           0,         /* options         */
                           0,         /* target_msc=next */
                           0, 0,      /* divisor, remainder */
                           0, NULL);  /* notifies        */
        xcb_flush(conn);

        uint8_t kind = 0;
        uint64_t msc = 0;
        int mode = wait_present_complete(conn, fserial, &kind, &msc);
        if (mode < 0) {
            fprintf(stderr, "flip test: no CompleteNotify for present %d\n", f);
            return 9;
        }
        got_any = 1;
        printf("  present %d: mode=%u (%s) msc=%llu\n",
               f, mode, present_mode_str(mode), (unsigned long long)msc);
        if (mode == XCB_PRESENT_COMPLETE_MODE_FLIP)
            got_flip = 1;
    }

    if (got_flip)
        printf("FLIP CONFIRMED: page-flip path engaged\n");
    else if (got_any)
        printf("flip not engaged (copy mode) — check for a running "
               "compositor or a pixmap/front-BO pitch mismatch\n");

    /* Cleanup. */
    xcb_free_pixmap(conn, fpix[0]);
    xcb_free_pixmap(conn, fpix[1]);
    xcb_destroy_window(conn, win);
    xcb_free_pixmap(conn, pix2);
    xcb_free_pixmap(conn, pix);
    free(br);
    xcb_flush(conn);
    xcb_disconnect(conn);
    printf("OK\n");
    return 0;
}

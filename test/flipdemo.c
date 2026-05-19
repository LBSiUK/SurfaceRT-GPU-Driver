/*
 * flipdemo — fullscreen Present page-flip demo for xf86-video-opentegra.
 *
 * Opens a fullscreen override-redirect window and continuously presents
 * a hue-cycled sweeping bar through the Present extension. When the
 * page-flip path (check_flip/flip) is engaged, each present is a
 * zero-copy drmModePageFlip scanout swap — the motion is tear-free and
 * locked to the 60 Hz LVDS refresh. A ~1 s readout reports the Present
 * completion mode and the achieved frame rate.
 *
 * Run at the lightdm greeter, or with the xfwm4 compositor disabled
 *   (xfconf-query -c xfwm4 -p /general/use_compositing -s false):
 * a running compositor redirects the window and forces copy mode.
 *
 *   flipdemo [seconds]      (default 15; Ctrl-C stops early)
 *
 * Build (armv7, inside the cross-build container):
 *   gcc -O2 -Wall -o flipdemo flipdemo.c \
 *       $(pkg-config --cflags --libs xcb xcb-present)
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <signal.h>
#include <time.h>

#include <xcb/xcb.h>
#include <xcb/present.h>

static volatile sig_atomic_t stop = 0;
static void on_signal(int s) { (void)s; stop = 1; }

static double now_sec(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

/* Block for the CompleteNotify of a given Present serial; returns its
 * mode (FLIP/COPY/...), or -1 on connection loss. Other events drained. */
static int
wait_complete(xcb_connection_t *conn, uint32_t want_serial)
{
    for (;;) {
        xcb_generic_event_t *ev = xcb_wait_for_event(conn);
        if (!ev)
            return -1;
        int mode = -1;
        if ((ev->response_type & 0x7f) == XCB_GE_GENERIC) {
            xcb_ge_generic_event_t *ge = (void *)ev;
            if (ge->event_type == XCB_PRESENT_COMPLETE_NOTIFY) {
                xcb_present_complete_notify_event_t *pe = (void *)ev;
                if (pe->serial == want_serial)
                    mode = pe->mode;
            }
        }
        free(ev);
        if (mode >= 0)
            return mode;
    }
}

/* Cheap hue sweep (deg 0..359) -> 0x00RRGGBB, for the bar colour. */
static uint32_t hue(int deg)
{
    deg %= 360; if (deg < 0) deg += 360;
    int region = deg / 60, f = (deg % 60) * 255 / 60;
    int r, g, b;
    switch (region) {
    case 0:  r = 255;   g = f;     b = 0;     break;
    case 1:  r = 255-f; g = 255;   b = 0;     break;
    case 2:  r = 0;     g = 255;   b = f;     break;
    case 3:  r = 0;     g = 255-f; b = 255;   break;
    case 4:  r = f;     g = 0;     b = 255;   break;
    default: r = 255;   g = 0;     b = 255-f; break;
    }
    return ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
}

int main(int argc, char **argv)
{
    double run_secs = (argc > 1) ? atof(argv[1]) : 15.0;

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

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

    uint16_t sw = screen->width_in_pixels;
    uint16_t sh = screen->height_in_pixels;
    uint8_t  sd = screen->root_depth;

    xcb_query_extension_reply_t *pqr = xcb_query_extension_reply(conn,
        xcb_query_extension(conn, 7, "Present"), NULL);
    if (!pqr || !pqr->present) {
        fprintf(stderr, "Present extension not available\n");
        return 2;
    }
    free(pqr);

    /* Fullscreen override-redirect window, raised to the top. */
    xcb_window_t win = xcb_generate_id(conn);
    uint32_t wv[2] = { 1 /*override_redirect*/,
                       XCB_EVENT_MASK_STRUCTURE_NOTIFY };
    xcb_create_window(conn, sd, win, screen->root, 0, 0, sw, sh, 0,
                      XCB_WINDOW_CLASS_INPUT_OUTPUT, screen->root_visual,
                      XCB_CW_OVERRIDE_REDIRECT | XCB_CW_EVENT_MASK, wv);
    xcb_map_window(conn, win);
    uint32_t raise = XCB_STACK_MODE_ABOVE;
    xcb_configure_window(conn, win, XCB_CONFIG_WINDOW_STACK_MODE, &raise);

    xcb_present_select_input(conn, xcb_generate_id(conn), win,
                             XCB_PRESENT_EVENT_MASK_COMPLETE_NOTIFY);

    /* Two screen-sized back buffers (alternated so each present targets
     * an idle one) plus a GC to draw with. */
    xcb_pixmap_t pix[2];
    for (int i = 0; i < 2; i++) {
        pix[i] = xcb_generate_id(conn);
        xcb_create_pixmap(conn, sd, pix[i], win, sw, sh);
    }
    xcb_gcontext_t gc = xcb_generate_id(conn);
    xcb_create_gc(conn, gc, pix[0], 0, NULL);
    xcb_flush(conn);

    /* Wait for the window to become viewable before presenting. */
    for (int t = 0; t < 20; t++) {
        xcb_generic_event_t *ev = xcb_wait_for_event(conn);
        if (!ev) break;
        int mapped = ((ev->response_type & 0x7f) == XCB_MAP_NOTIFY);
        free(ev);
        if (mapped) break;
    }

    printf("flipdemo: %ux%u depth=%u — running %.0fs (Ctrl-C to stop)\n",
           sw, sh, sd, run_secs);

    const uint16_t barw = 96;
    uint32_t serial = 0;
    long frame = 0, flips = 0;
    double t0 = now_sec(), t_report = t0;
    long report_frame0 = 0;
    int buf = 0;

    while (!stop && now_sec() - t0 < run_secs) {
        xcb_pixmap_t p = pix[buf];

        /* Draw: dark background + a hue-cycled bar sweeping left->right. */
        int barx = (int)((frame * 12) % (sw + barw)) - barw;

        uint32_t v = 0x101018;                  /* background */
        xcb_rectangle_t full = { 0, 0, sw, sh };
        xcb_change_gc(conn, gc, XCB_GC_FOREGROUND, &v);
        xcb_poly_fill_rectangle(conn, p, gc, 1, &full);

        v = hue((int)frame);                    /* bar */
        xcb_rectangle_t bar = { (int16_t)barx, 0, barw, sh };
        xcb_change_gc(conn, gc, XCB_GC_FOREGROUND, &v);
        xcb_poly_fill_rectangle(conn, p, gc, 1, &bar);

        /* Present it — a scanout swap when the flip path is engaged. */
        uint32_t s = ++serial;
        xcb_present_pixmap(conn, win, p, s,
                           XCB_NONE,  /* valid region    */
                           XCB_NONE,  /* update region   */
                           0, 0,      /* x_off, y_off     */
                           XCB_NONE,  /* target_crtc      */
                           XCB_NONE,  /* wait_fence       */
                           XCB_NONE,  /* idle_fence       */
                           0,         /* options          */
                           0,         /* target_msc=next  */
                           0, 0,      /* divisor, remainder */
                           0, NULL);  /* notifies         */
        xcb_flush(conn);

        int mode = wait_complete(conn, s);
        if (mode < 0) { fprintf(stderr, "\nconnection lost\n"); break; }
        if (mode == XCB_PRESENT_COMPLETE_MODE_FLIP)
            flips++;

        frame++;
        buf ^= 1;

        double tn = now_sec();
        if (tn - t_report >= 1.0) {
            printf("\r  frame %-6ld  mode=%-4s  flips=%ld/%ld  %.1f fps  ",
                   frame,
                   mode == XCB_PRESENT_COMPLETE_MODE_FLIP ? "FLIP" :
                   mode == XCB_PRESENT_COMPLETE_MODE_COPY ? "COPY" : "?",
                   flips, frame,
                   (frame - report_frame0) / (tn - t_report));
            fflush(stdout);
            t_report = tn;
            report_frame0 = frame;
        }
    }

    double total = now_sec() - t0;
    printf("\n--- %ld frames in %.1fs (%.1f fps avg), %ld flipped (%.0f%%) ---\n",
           frame, total, frame / (total > 0 ? total : 1),
           flips, frame ? 100.0 * flips / frame : 0.0);
    if (frame && flips == frame)
        printf("PAGE-FLIP: every frame flipped — tear-free vsync scanout.\n");
    else if (flips)
        printf("PAGE-FLIP: partial — %ld/%ld frames flipped.\n", flips, frame);
    else
        printf("PAGE-FLIP: not engaged (copy mode) — a desktop compositor "
               "is likely redirecting the window.\n");

    /* Cleanup — destroying the window makes Present unflip. */
    xcb_free_gc(conn, gc);
    xcb_free_pixmap(conn, pix[0]);
    xcb_free_pixmap(conn, pix[1]);
    xcb_destroy_window(conn, win);
    xcb_flush(conn);
    xcb_disconnect(conn);
    return 0;
}

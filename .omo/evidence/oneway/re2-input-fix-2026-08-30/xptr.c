/* xptr.c - pointer-only XTest pass-through probe (NO keypress: safe to run
 * on a live desktop - it never types into the user's focused window).
 *
 * baseline : game window only -> does the game get the pointer button/motion?
 * overlay  : + fullscreen input=False window on top -> does the game still get
 *            the pointer? The overlay ALSO selects button events so we can
 *            confirm it is the one capturing the pointer (vs. a failed warp).
 *
 * Counts events (not XQueryPointer readback: Xwayland updates the pointer
 * position asynchronously, so the readback lags the actual warp).
 */
#define _DEFAULT_SOURCE
#include <X11/Xlib.h>
#include <X11/extensions/XTest.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/select.h>

int main(int argc, char** argv) {
    const bool withOverlay = (argc > 1) && strcmp(argv[1], "overlay") == 0;
    Display* d = XOpenDisplay(NULL);
    if (!d) { fprintf(stderr, "cannot open $DISPLAY\n"); return 2; }
    const int screen = DefaultScreen(d);
    const Window root = RootWindow(d, screen);
    const int winW = 1280, winH = 720;

    const Window gw = XCreateSimpleWindow(d, root, 0, 0, winW, winH, 0,
                                          BlackPixel(d, screen), BlackPixel(d, screen));
    XSelectInput(d, gw, ButtonPressMask | ButtonReleaseMask | PointerMotionMask);
    XStoreName(d, gw, "xptr-game");
    XMapWindow(d, gw);
    XSync(d, True);
    usleep(800000);

    Window ow = None;
    if (withOverlay) {
        ow = XCreateSimpleWindow(d, root, 0, 0,
                                 DisplayWidth(d, screen), DisplayHeight(d, screen), 0,
                                 BlackPixel(d, screen), BlackPixel(d, screen));
        /* mirror backend_x11.cpp chrome: WM_HINTS input=False + UTILITY */
        const long hints[2] = {2L /*InputHint*/, 0L /*input=False*/};
        const Atom hintsAtom = XInternAtom(d, "WM_HINTS", True);
        XChangeProperty(d, ow, hintsAtom, hintsAtom, 32, PropModeReplace,
                        (const unsigned char*)hints, 2);
        const Atom typeAtom = XInternAtom(d, "_NET_WM_WINDOW_TYPE", False);
        const Atom utilAtom = XInternAtom(d, "_NET_WM_WINDOW_TYPE_UTILITY", False);
        XChangeProperty(d, ow, typeAtom, typeAtom, 32, PropModeReplace,
                        (const unsigned char*)&utilAtom, 1);
        XSelectInput(d, ow, ButtonPressMask);  /* so we can see it capturing */
        XStoreName(d, ow, "xptr-overlay");
        XMapWindow(d, ow);
        XSync(d, True);
        usleep(800000);
    }

    /* drain startup events */
    {
        const int fd = XConnectionNumber(d);
        fd_set fds; struct timeval tv = {0, 200000};
        for (int i = 0; i < 100; i++) {
            FD_ZERO(&fds); FD_SET(fd, &fds);
            if (select(fd + 1, &fds, NULL, NULL, &tv) > 0) { XEvent ev; XMaskEvent(d, ~0L, &ev); }
            else break;
        }
    }

    int gameRx = 0, gameRy = 0, realW = winW, realH = winH;
    {
        Window gc; int gx = 0, gy = 0; unsigned bw = 0, bd = 0;
        XTranslateCoordinates(d, gw, root, 0, 0, &gx, &gy, &gc);
        XGetGeometry(d, gw, &gc, &gx, &gy, &realW, &realH, &bw, &bd);
        gameRx = gx; gameRy = gy;
    }
    const int targetX = gameRx + realW / 2, targetY = gameRy + realH / 2;
    printf("geom: game at root (%d,%d) size %dx%d -> target (%d,%d)\n",
           gameRx, gameRy, realW, realH, targetX, targetY);

    int origX = 0, origY = 0;
    {
        Window rr0, cr0; int ox = 0, oy = 0, wx = 0, wy = 0; unsigned m0;
        XQueryPointer(d, root, &rr0, &cr0, &ox, &oy, &wx, &wy, &m0);
        origX = ox; origY = oy;
    }

    /* warp + inject button press/release + a motion */
    XTestFakeMotionEvent(d, -1, targetX, targetY, 0);
    XSync(d, True);
    XTestFakeButtonEvent(d, 1, True, 0);
    XTestFakeButtonEvent(d, 1, False, 0);
    XTestFakeMotionEvent(d, -1, targetX + 1, targetY + 1, 0);
    XSync(d, True);

    int gBtn = 0, gMot = 0, oBtn = 0;
    {
        const int fd = XConnectionNumber(d);
        fd_set fds; struct timeval tv = {0, 30000};
        for (long i = 0; i < 120; i++) {
            FD_ZERO(&fds); FD_SET(fd, &fds);
            int sel = select(fd + 1, &fds, NULL, NULL, &tv);
            if (sel <= 0) { if (sel == 0) break; continue; }
            XEvent ev; XMaskEvent(d, ~0L, &ev);
            if (ev.xany.window == gw) {
                if (ev.type == ButtonPress) gBtn++;
                else if (ev.type == MotionNotify) gMot++;
            } else if (ow != None && ev.xany.window == ow) {
                if (ev.type == ButtonPress) oBtn++;
            }
        }
    }
    printf("mode=%s  GAME received: button=%d motion=%d | OVERLAY received: button=%d\n",
           withOverlay ? "overlay" : "baseline", gBtn, gMot, oBtn);

    /* restore the user's pointer */
    XTestFakeMotionEvent(d, -1, origX, origY, 0);
    XSync(d, True);

    if (ow != None) XDestroyWindow(d, ow);
    XDestroyWindow(d, gw);
    XCloseDisplay(d);
    return gBtn >= 1 ? 0 : 1;
}
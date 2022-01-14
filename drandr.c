/* See LICENSE file for copyright and license details. */
#include <ctype.h>
#include <locale.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>
#include <stdint.h>

#include <sys/wait.h>
#include <sys/file.h>
#include <errno.h>

#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <X11/Xutil.h>

#ifdef XINERAMA
#include <X11/extensions/Xinerama.h>
#endif

#include <X11/extensions/Xrandr.h>

#include <X11/Xft/Xft.h>


#include "drw.h"
#include "util.h"

#define INTERSECT(x, y, w, h, r)  (MAX(0, MIN((x)+(w),(r).x_org+(r).width)  - MAX((x),(r).x_org)) \
                             && MAX(0, MIN((y)+(h),(r).y_org+(r).height) - MAX((y),(r).y_org)))
#define LENGTH(X)             (sizeof (X) / sizeof (X)[0])
#define TEXTW(X)                (drw_fontset_getwidth(drw, (X)) + lrpad)

enum {
    SchemeSel, SchemeMon, SchemeNorm, SchemeLast
};
static int bh, mw, mh, lrpad;
static int mon = -1, screen;

static int st, sr, sb, sl; // top, right, bottom, left of screen
static int mcw, mch; // maximum possible screen size (w & h indepentent)
static double canvas_scale = 1.0/12;

static Display *dpy;
static Window root, parentWin, win;
static XIC xic;
static int win_output;

static Drw *drw;
static Clr *scheme[SchemeLast];

#include "config.h"


static Display *dpy;
static Window root;

static char buf[32];

typedef struct Button Button;
struct Button {
    int x,y,w,h;
    char *text;
    void (*callback)(void);
};
Button* hovered_button;

#define EVENT_SIZE 128
#define EDID_SIZE 513
#define SCREENID_SIZE 3

typedef struct OutputConnection OutputConnection;
struct OutputConnection {
    RROutput output;
    const char *edid;
    XRROutputInfo *info;
    XRRCrtcInfo *crtc_info;
    OutputConnection *next;
    OutputConnection *prev;

    int cx, cy, cw, ch; // positions and sizes in canvas (gui), scaled down and translated from real values
    int x, y, w, h; // new, "real" positions and sizes

};

typedef struct CrtcWindow CrtcWindow;
struct CrtcWindow {
    Window win;
    RRCrtc ctrc;
};
CrtcWindow* crtc_wins;
int n_crtc_wins;

static OutputConnection *head;

static OutputConnection *grabbed;
int grabbed_offset_x, grabbed_offset_y;

void remove_output_connection(OutputConnection *ocon);
static void reset_canvas_positions();
static void update_total_screen_size();
static void update_window_size();
static void recenter_canvas();
static void update_canvas();
static void apply();
static void create_crtc_windows(XRRScreenResources *sres);

Button button_apply = {0, 0, 100, 12, "Apply", apply};
Button* buttons[] = {&button_apply};

static void cleanup(void) {
    while (head) {
        remove_output_connection(head);
    }
    if (dpy) {
        XSync(dpy, False);
        XCloseDisplay(dpy);
        dpy = NULL;
    }
}


void free_output_connection(OutputConnection *ocon) {
    if (ocon) {
        if (ocon == grabbed) {
            grabbed = NULL;
        }
        XRRFreeOutputInfo(ocon->info);
        XRRFreeCrtcInfo(ocon->crtc_info);
        free((char*) ocon->edid);
        free(ocon);
    }
}

OutputConnection *get_output_connection(RROutput output) {
    OutputConnection *ocon = head;
    for (; ocon && ocon->output != output; ocon = ocon->next);
    return ocon;
}

void remove_output_connection(OutputConnection *ocon) {
    if (ocon == head) {
        head = ocon->next;
    } else {
        ocon->prev->next = ocon->next;
    }

    free_output_connection(ocon);
}

void append_output_connection(OutputConnection *ocon) {
    if (!head) {
        head = ocon;
        return;
    }
    OutputConnection *last_ocon = head;
    for (; last_ocon && last_ocon->next; last_ocon = last_ocon->next);
    last_ocon->next = ocon;
    ocon->prev = last_ocon;
}

//int get_sid(Display *dpy, RROutput output) {
//    XRRMonitorInfo *mi;
//    XRRScreenResources *sres;
//    XineramaScreenInfo *si;
//    int i, j, k, sid = -1;
//    int nmonitors, nscreens;
//
//    si = XineramaQueryScreens(dpy, &nscreens);
//    mi = XRRGetMonitors(dpy, DefaultRootWindow(dpy), True, &nmonitors);
//    sres = XRRGetScreenResourcesCurrent(dpy, DefaultRootWindow(dpy));
//
//    for (j = 0; j < nmonitors; ++j) {
//        for (k = 0; k < mi[j].noutput && mi[j].outputs[k] != output; ++k) {
//            ;
//        }
//        if (k < mi[j].noutput) {
//            break;
//        }
//    }
//
//    if (si && mi && j < nmonitors) {
//        for (i = 0; i < nscreens; ++i) {
//            if (si[i].x_org == mi[j].x &&
//                si[i].y_org == mi[j].y && si[i].width == mi[j].width && si[i].height == mi[j].height) {
//                sid = i;
//                break;
//            }
//        }
//        XFree(si);
//        XRRFreeMonitors(mi);
//    }
//    XRRFreeScreenResources(sres);
//
//    return sid;
//}

const char *get_edid(Display* dpy, RROutput out) {
    int props = 0;
    Atom *properties;
    Atom atom_edid, real;
    int i, format;
//    uint16_t vendor, product;
//    uint32_t serial;
    char *edid = NULL;
    unsigned char *p;
    char *ep;
    unsigned long n, extra;
    Bool has_edid_prop = False;


    properties = XRRListOutputProperties(dpy, out, &props);
    atom_edid = XInternAtom(dpy, RR_PROPERTY_RANDR_EDID, False);
    for (i = 0; i < props; i++) {
        if (properties[i] == atom_edid) {
            has_edid_prop = True;
            break;
        }
    }
    XFree(properties);

    if (has_edid_prop) {
        if (XRRGetOutputProperty(dpy, out, atom_edid, 0L, 128L, False, False,
                                 AnyPropertyType, &real, &format, &n, &extra, &p) == Success) {
            if (n >= 127) {
//                vendor = (p[9] << 8) | p[8];
//                product = (p[11] << 8) | p[10];
//                serial = p[15] << 24 | p[14] << 16 | p[13] << 8 | p[12];
//                snprintf(edid, edidlen, "%04X%04X%08X", vendor, product, serial);
                edid = ecalloc(2*n+1, sizeof(char));
                ep = edid;
                for (i = 0; i < n; i++) {
                    snprintf(ep, EDID_SIZE-(ep-edid),"%02x", p[i]);
                    ep += 2;
                }
                *ep = 0;
            }
            free(p);
        }
    }
    return edid;
}

OutputConnection *create_output_connection(RROutput output, XRROutputInfo *info) {
    XRRScreenResources *sres;
    XRRModeInfo *mode_info;
    OutputConnection *ocon;
    const char* edid;
    int i;

    sres = XRRGetScreenResourcesCurrent(dpy, DefaultRootWindow(dpy));

    if (!info) {
        info = XRRGetOutputInfo(dpy, sres, output);
    }

    edid = get_edid(dpy, output);

    for (ocon = head; ocon;) {
        if (edid && ocon->edid && strcmp(edid, ocon->edid) == 0) {
            remove_output_connection(ocon);
            ocon = head; //TODO: this is a bad loop
        } else {
            ocon = ocon->next;
        }
    }

    ocon = ecalloc(1, sizeof(OutputConnection));
    memset(ocon, 0, sizeof(OutputConnection));
    ocon->output = output;

    ocon->edid = edid;

    ocon->info = info;
    if (info->crtc) {
        ocon->crtc_info = XRRGetCrtcInfo(dpy, sres, info->crtc);
        ocon->x = ocon->crtc_info->x;
        ocon->y = ocon->crtc_info->y;
        ocon->w = ocon->crtc_info->width;
        ocon->h = ocon->crtc_info->height;
    } else {
        ocon->crtc_info = NULL;

        for (i = 0; i < sres->nmode; i++) {
            if (sres->modes[i].id == ocon->info->modes[0]) {
                mode_info = &sres->modes[i];
                break;
            }
        }
        if (mode_info) {
            ocon->x = 0;
            ocon->y = 0;
            ocon->w = (int) mode_info->width;
            ocon->h = (int) mode_info->height;
        }
    }

    append_output_connection(ocon);
    XRRFreeScreenResources(sres);
    return ocon;
}


void get_outputs() {
    XRRScreenResources *sres;
    XRROutputInfo *info;
    int i;

    sres = XRRGetScreenResources(dpy, root);
    if (!sres) {
        fprintf(stderr, "Could not get screen resources\n");
        return;
    }
    for (i = 0; i < sres->noutput; i++) {
        info = XRRGetOutputInfo(dpy, sres, sres->outputs[i]);
        if (info->connection == RR_Connected) {
            create_output_connection(sres->outputs[i], info);
        } else {
            XRRFreeOutputInfo(info);
        }
    }
    create_crtc_windows(sres);

    XRRFreeScreenResources(sres);
}



static void handle_output_change_event(XRROutputChangeNotifyEvent *ev) {
    XRRScreenResources *sres;
    XRROutputInfo *info;
    OutputConnection *ocon;

    sres = XRRGetScreenResourcesCurrent(ev->display, ev->window);
    if (!sres) {
        fprintf(stderr, "Could not get screen resources\n");
        return;
    }
    printf("handle_output_change_event.XRRGetOutputInfo %d\n", ev->output);
    fflush(stdout);
    info = XRRGetOutputInfo(ev->display, sres, ev->output);
    if (!info) {
        XRRFreeScreenResources(sres);
        fprintf(stderr, "Could not get output info\n");
        return;
    }


    switch (info->connection) {
        case RR_Connected:
            ocon = create_output_connection(ev->output, NULL);
            printf("connected %s (EDID: %s)\n", ocon->info->name, ocon->edid);
            break;
        case RR_Disconnected:
            ocon = get_output_connection(ev->output);
            if (ocon) {
                printf("disconnected %s (EDID: %s)\n", ocon->info->name, ocon->edid);
                remove_output_connection(ocon);
            } else {
                printf("disconnected %s (EDID: unknown)\n", info->name);
            }
            break;
        default:
            printf("unknown connection code %d\n", info->connection);
            break;
    }

    update_canvas();

    XRRFreeScreenResources(sres);
    XRRFreeOutputInfo(info);
}

static void handle_randr_event(XRRNotifyEvent* ev) {
    switch (ev->subtype) {
        case RRNotify_OutputChange:
            handle_output_change_event((XRROutputChangeNotifyEvent *) ev);
            break;
        default:
            break;
    }
}

static void setup_new_coordinates(int *screen_width, int *screen_height, int *screen_width_mm, int *screen_height_mm,
                                  double *dpi) {
    OutputConnection *reference, *ocon;
    int nst = INT_MAX, nsr = INT_MIN, nsb = INT_MIN, nsl = INT_MAX; // new screen top, right, bottom, left

    for (reference = head; reference && !reference->crtc_info; reference = reference->next) {}
    if (!reference) {
        die("no output with ctrc found");
    }

    *dpi = (25.4 * reference->crtc_info->height) / (double) reference->info->mm_height;

    for (ocon = head; ocon; ocon = ocon->next) {
        if (ocon->info->connection == RR_Disconnected) continue;
        ocon->x = (int) ((ocon->cx - reference->cx) / canvas_scale);
        ocon->y = (int) ((ocon->cy - reference->cy) / canvas_scale);
        int ocun_right = (int) ocon->x + ocon->w;
        int ocun_bottom = (int) ocon->y + ocon->h;
        nsl = MIN(nsl, ocon->x);
        nsr = MAX(nsr, ocun_right);
        nst = MIN(nst, ocon->y);
        nsb = MAX(nsb, ocun_bottom);
    }

    for (ocon = head; ocon; ocon = ocon->next) {
        ocon->x -= nsl;
        ocon->y -= nst;
    }

    nsr -= nsl;
    nsb -= nst;
    nsl = 0;
    nst = 0;

    *screen_width = nsr;
    *screen_width_mm = (int) ((25.4 * (double)(*screen_width)) / (*dpi));
    *screen_height = nsb;
    *screen_height_mm = (int) ((25.4 * (double)(*screen_height)) / (*dpi));
}

static void disable_crtc(XRRScreenResources *sres, RRCrtc ctrc) {
    XRRSetCrtcConfig (dpy, sres, ctrc, CurrentTime,
                      0, 0, None, RR_Rotate_0, NULL, 0);
    printf("disbaled crtc %d\n", ctrc);
}

static void disable_unused_crtcs(XRRScreenResources *sres, int screen_width, int screen_height) {
    OutputConnection *ocon;
    XRRCrtcInfo *crtc_info;
    XRROutputInfo *output_info;
    int i, o;
    Bool output_connected;

    for (i = 0; i < sres->ncrtc; i++) {
        crtc_info = XRRGetCrtcInfo(dpy, sres, sres->crtcs[i]);

        if (crtc_info->mode == None) {
            XRRFreeCrtcInfo(crtc_info);
            continue;
        };

        // disable if not in screen or no output assigned
        if (crtc_info->x + crtc_info->width > screen_width
                || crtc_info->y + crtc_info->height > screen_height
                || crtc_info->noutput == 0) {
            disable_crtc(sres, sres->crtcs[i]);
        } else if (crtc_info->noutput > 0) {
            // disable if no assigned output is connected
            output_connected = False;
            for (o = 0; o < crtc_info->noutput; o++) {
                output_info = XRRGetOutputInfo(dpy, sres, crtc_info->outputs[o]);
                if (output_info->connection != RR_Disconnected) {
                    output_connected = True;
                    XRRFreeOutputInfo(output_info);
                    break;
                }
                XRRFreeOutputInfo(output_info);
            }
            if (output_connected == False) {
                disable_crtc(sres, sres->crtcs[i]);
            }
        }
        XRRFreeCrtcInfo(crtc_info);
    }
}

static Status add_output_to_unused_crtc(XRRScreenResources *sres, OutputConnection* ocon) {
    Status s;
    int i;
    XRRCrtcInfo *crtc_info;

    s = RRSetConfigFailed;

    for (i = 0; i < sres->ncrtc; i++) {
        crtc_info = XRRGetCrtcInfo(dpy, sres, sres->crtcs[i]);

        if (crtc_info->noutput > 0) {
            XRRFreeCrtcInfo(crtc_info);
            continue;
        };

        RROutput *outputs = ecalloc(1, sizeof(RROutput));
        outputs[0] = ocon->output;

        printf("Setting: %lu %s: crtc: %lu mode: %lu\n", ocon->output, ocon->info->name, sres->crtcs[i], ocon->info->modes[0]);

        s = XRRSetCrtcConfig(dpy, sres, sres->crtcs[i], CurrentTime,
                             ocon->x, ocon->y, ocon->info->modes[0], RR_Rotate_0,
                             outputs, 1);
        XRRFreeCrtcInfo(crtc_info);
        break;
    }
    return s;
}

static void apply_output(XRRScreenResources *sres, OutputConnection* ocon) {
    Status s;

    s = RRSetConfigFailed;

    if (ocon->crtc_info) {
        printf("Setting: %lu %s: crtc: %lu mode: %lu\n", ocon->output, ocon->info->name, ocon->info->crtc, ocon->crtc_info->mode);

        s = XRRSetCrtcConfig (dpy, sres, ocon->info->crtc, CurrentTime,
                              ocon->x, ocon->y, ocon->crtc_info->mode, ocon->crtc_info->rotation,
                              ocon->crtc_info->outputs, ocon->crtc_info->noutput);
    } else {
       s = add_output_to_unused_crtc(sres, ocon);
    }

    if (s == RRSetConfigSuccess) {
        printf("Success: %lu %s\n", ocon->output, ocon->info->name);
    } else {
        fprintf(stderr, "Error: %lu %s\n", ocon->output, ocon->info->name);
    }
}

static void apply() {
    OutputConnection *ocon;
    XRRScreenResources *sres;
    int screen_width, screen_height, screen_width_mm, screen_height_mm;
    double dpi;
    setup_new_coordinates(&screen_width, &screen_height, &screen_width_mm, &screen_height_mm, &dpi);
    sres = XRRGetScreenResourcesCurrent(dpy, DefaultRootWindow(dpy));
    printf("Apply\n");

    XGrabServer(dpy);

    disable_unused_crtcs(sres, screen_width, screen_height);

    printf("screen %d: %dx%d %dx%d mm %6.2fdpi\n", screen,
           screen_width, screen_height, screen_width_mm, screen_height_mm, dpi);
    fflush(stdout);
    XRRSetScreenSize(dpy, root, screen_width, screen_height,
                      screen_width_mm, screen_height_mm);


    for (ocon = head; ocon; ocon = ocon->next) {
        apply_output(sres, ocon);
    }

    XUngrabServer(dpy);
    XRRFreeScreenResources(sres);
    XSync(dpy, False);
    get_outputs();
    update_canvas();
}

static void update_canvas() {
    update_total_screen_size();
    update_window_size();
    reset_canvas_positions();
    recenter_canvas();
}

static void reset_canvas_positions() {
    OutputConnection *ocon;
    double canvas_offset_x, canvas_offset_y;

    canvas_offset_x = (double) (sr-sl)/2;
    canvas_offset_y = (double) (sb-st)/2;

    for (ocon = head; ocon; ocon = ocon->next) {

        ocon->cw = (int) ((double) ocon->w * canvas_scale);
        ocon->ch = (int) ((double) ocon->h * canvas_scale);

        ocon->cx = (int) (((double) ocon->x - canvas_offset_x) * canvas_scale) + mw/2;
        ocon->cy = (int) (((double) ocon->y - canvas_offset_y) * canvas_scale) + mh/2;
    }
}

static void recenter_canvas() {
    int ct=mh, cr=0, cb=0, cl=mw, c_offset_x, c_offset_y;
    OutputConnection *ocon;
    for (ocon = head; ocon; ocon = ocon->next) {
        ct = MIN(ct, ocon->cy);
        cr = MAX(cr, ocon->cx + ocon->cw);
        cb = MAX(cb, ocon->cy + ocon->ch);
        cl = MIN(cl, ocon->cx);
    }
    c_offset_x = (cl + cr)/2 - mw/2;
    c_offset_y = (ct + cb)/2 - mh/2;
    for (ocon = head; ocon; ocon = ocon->next) {
        ocon->cx -= c_offset_x;
        ocon->cy -= c_offset_y;
    }
}

static void snap_output(OutputConnection *ocon) {
    int axis = -1; // 0 = snap along x axis, 1 = snap along y axis
    int dist, min_dist;
    min_dist = INT_MAX;
    OutputConnection *snap_target, *iter;

    for (iter = head; iter; iter = iter->next) {
        if (iter == ocon) continue;
        if (iter->cx + iter->cw >= ocon->cx && iter->cx <= ocon->cx + ocon->cw) {
            if (ocon->cy >= iter->cy) {
                dist = abs(ocon->cy - iter->cy - iter->ch);
            } else {
                dist = abs(iter->cy - ocon->cy - ocon->ch);
            }
            if (dist < min_dist) {
                axis = 1;
                min_dist = dist;
                snap_target = iter;
            }
        }

        if (iter->cy + iter->ch >= ocon->cy && iter->cy <= ocon->cy + ocon->ch) {
            if (ocon->cx >= iter->cx) {
                dist = abs(ocon->cx - iter->cx - iter->cw);
            } else {
                dist = abs(iter->cx - ocon->cx - ocon->cw);
            }
            if (dist < min_dist) {
                axis = 0;
                min_dist = dist;
                snap_target = iter;
            }
        }
    }

    if (axis == 0) {
        if (ocon->cx >= snap_target->cx) {
            ocon->cx = snap_target->cx + snap_target->cw;
        } else {
            ocon->cx = snap_target->cx - ocon->cw;
        }
        if (abs(ocon->cy - snap_target->cy) < 10 || abs(ocon->cy + ocon->ch - snap_target->cy - snap_target->ch) < 10) {
            ocon->cy = snap_target->cy;
        }
    } else if (axis == 1) {
        if (ocon->cy >= snap_target->cy) {
            ocon->cy = snap_target->cy + snap_target->ch;
        } else {
            ocon->cy = snap_target->cy - ocon->ch;
        }
        if (abs(ocon->cx - snap_target->cx) < 10 || abs(ocon->cx + ocon->cw - snap_target->cx - snap_target->cw) < 10) {
            ocon->cx = snap_target->cx;
        }
    }
}

static void update_total_screen_size() {
    OutputConnection* ocon;
    mcw = 0;
    mch = 0;
    for (ocon = head; ocon; ocon = ocon->next) {
        int cl = ocon->x;
        int cr = cl + (int) ocon->w;

        int ct = ocon->y;
        int cb = ct + (int) ocon->h;

        sl = MIN(sl, cl);
        sr = MAX(sr, cr);
        st = MIN(st, ct);
        sb = MAX(sb, cb);

        mcw += (int) ocon->w;
        mch += (int) ocon->h;
    }
}


static void update_window_size() {
    int new_x, new_y;
    double canvas_scale_x, canvas_scale_y;
    XWindowAttributes wa;
    XWindowChanges wc;
    OutputConnection *win_ocon;

    if (head) {
        XGetWindowAttributes(dpy, win, &wa);

//        mw = (int) (((double) mcw * 2) * canvas_scale);
//        mh = (int) (((double) mch * 2) * canvas_scale);
        canvas_scale_x = mw/((double) mcw * 2);
        canvas_scale_y = mh/((double) mch * 2);
        canvas_scale = MIN(canvas_scale_x, canvas_scale_y);

        for (win_ocon = head; win_ocon && win_ocon->output != win_output; win_ocon = win_ocon->next) {}
        if (!win_ocon) {
            win_ocon = head;
        }
        printf("%d", win_ocon->crtc_info ? 1:0);
        fflush(stdout);
        new_x = win_ocon->crtc_info->x + win_ocon->crtc_info->width / 2 - mw / 2;
        new_y = win_ocon->crtc_info->y + win_ocon->crtc_info->height / 2 - mh / 2;

        if (mw != wa.width || mh != wa.height || new_x != wa.x || new_y != wa.y) {

            wc.x = new_x;
            wc.y = new_y;
            wc.width = mw;
            wc.height = mh;

            XConfigureWindow(dpy, win, CWX | CWY | CWWidth | CWHeight, &wc);
            drw_resize(drw, mw, mh);

            reset_canvas_positions();

        }
    }

    button_apply.h = bh;
    button_apply.y = mh - bh;
    button_apply.x = mw - TEXTW(button_apply.text);

}

static void draw_output(OutputConnection *ocon) {
    int bw=2, y=ocon->cy;
    int boxs = drw->fonts->h / 9;
    int boxw = drw->fonts->h / 6 + 2;

    drw_setscheme(drw, ocon == grabbed ? scheme[SchemeSel] : scheme[SchemeMon]);
    drw_rect(drw, ocon->cx, y, ocon->cw, ocon->ch, 1, 0);
    drw_rect(drw, ocon->cx+1, y+1, ocon->cw-2, ocon->ch-2, 1, 1);


    y += 1;
    drw_text(drw, ocon->cx+1, y, ocon->cw-2-bw, bh, lrpad/2, ocon->info->name, 0);

    drw_setscheme(drw, scheme[SchemeNorm]);
    y += bh;
    snprintf(buf, sizeof buf, "%dx%d", ocon->w, ocon->h);
    drw_text(drw, ocon->cx+1, y, ocon->cw-2-bw, bh, lrpad/2, buf, 0);

    if (ocon->crtc_info) {
        drw_rect(drw, ocon->cx + boxs, ocon->cy + boxs, boxw, boxw, ocon->output == win_output, 0);
    }

    y += bh;
    drw_rect(drw, ocon->cx+1+bw, y, ocon->cw-2-2*bw, ocon->ch-1-1*bw - (y - ocon->cy), 1, 1);
    if (bw > 0) {
        drw_setscheme(drw, ocon == grabbed ? scheme[SchemeSel] : scheme[SchemeMon]);
        drw_rect(drw, ocon->cx+1, ocon->cy+1+bh, bw, ocon->ch-1-bh-1*bw, 1, 1);
    }
}

static void draw(void) {
    int i;

    drw_setscheme(drw, scheme[SchemeNorm]);
    drw_rect(drw, 0, 0, mw, mh, 1, 1);


    for (OutputConnection* ocon = head; ocon; ocon = ocon->next) {
        if (ocon != grabbed) {
            draw_output(ocon);
        }
    }
    if (grabbed) {
        draw_output(grabbed);
    }

    for (i = 0; i < LENGTH(buttons); i++) {
        drw_setscheme(drw, buttons[i] == hovered_button ? scheme[SchemeMon] : scheme[SchemeNorm]);
        drw_text(drw, buttons[i]->x, buttons[i]->y, buttons[i]->w, buttons[i]->h, lrpad/2, buttons[i]->text, 0);
    }
    drw_map(drw, win, 0, 0, mw, mh);
}

static void buttonpress(XButtonPressedEvent *e) {
    int x, y, i;
    x = e->x;
    y = e->y;

    if (e->button == 1) {
        for (OutputConnection* ocon = head; ocon; ocon = ocon->next) {
            if (x >= ocon->cx && x <= ocon->cx + ocon->cw
                && y >= ocon->cy && y <= ocon->cy + ocon->ch) {
                grabbed = ocon;
                grabbed_offset_x = ocon->cx - x;
                grabbed_offset_y = ocon->cy - y;
            }
        }
        if (!grabbed) {
            for (i = 0; i < LENGTH(buttons); i++) {
                if (x >= buttons[i]->x && x <= buttons[i]->x + buttons[i]->w
                    && y >= buttons[i]->y && y <= buttons[i]->y + buttons[i]->h) {
                    buttons[i]->callback();
                }
            }
        }
    }
}

static void buttonrelease(XButtonReleasedEvent *e) {
    if (e->button == 1) {
        if (grabbed) {
            snap_output(grabbed);
            grabbed = NULL;
            recenter_canvas();
        }
    }
}

static void motion(XPointerMovedEvent *e) {
    int x, y, i;
    x = e->x;
    y = e->y;

    if (grabbed) {
        grabbed->cx = x + grabbed_offset_x;
        grabbed->cy = y + grabbed_offset_y;
    } else {
        hovered_button = NULL;
        for (i = 0; i < LENGTH(buttons); i++) {
            if (x >= buttons[i]->x && x <= buttons[i]->x + buttons[i]->w
                && y >= buttons[i]->y && y <= buttons[i]->y + buttons[i]->h) {
                hovered_button = buttons[i];
                break;
            }
        }
    }
}

static void keypress(XKeyEvent *ev) {
    KeySym ksym;
    Status status;

    XmbLookupString(xic, ev, buf, sizeof buf, &ksym, &status);
    switch (status) {
        default: /* XLookupNone, XBufferOverflow */
            return;
        case XLookupKeySym:
        case XLookupBoth:
            break;
    }
    switch (ksym) {
        default:
            return;
        case XK_Escape:
        case XK_q:
            exit(0);
    }
}

static void handle_events() {
    XEvent ev;

    while (XPending(dpy) && !XNextEvent(dpy, &ev)) {
        switch (ev.type) {
            case ButtonPress:
                buttonpress(&ev.xbutton);
                break;
            case ButtonRelease:
                buttonrelease(&ev.xbutton);
                break;
            case MotionNotify:
                motion(&ev.xmotion);
                break;
            case KeyPress:
                keypress(&ev.xkey);
                break;
            case 90: // Randr event
                handle_randr_event((XRRNotifyEvent *) &ev);
                break;
        }
        fflush(stdout);
    }
}

static void run(void) {
    struct timespec start, current, diff, interval_spec, wait;
    timespec_set_ms(&interval_spec, interval);

    for (;;) {

        if (clock_gettime(CLOCK_MONOTONIC, &start) < 0) {
            die("clock_gettime:");
        }

        handle_events();
        draw();

        if (clock_gettime(CLOCK_MONOTONIC, &current) < 0) {
            die("clock_gettime:");
        }

        timespec_diff(&diff, &current, &start);
        timespec_diff(&wait, &interval_spec, &diff);

        if (wait.tv_sec >= 0 && wait.tv_nsec >= 0) {
            if (nanosleep(&wait, NULL) < 0 && errno != EINTR) {
                die("nanosleep:");
            }
        }
    }
    cleanup();
}


static void grab_focus(void) {
    struct timespec ts = {.tv_sec = 0, .tv_nsec = 10000000};
    Window focuswin;
    int i, revertwin;

    for (i = 0; i < 100; ++i) {
        XGetInputFocus(dpy, &focuswin, &revertwin);
        if (focuswin == win)
            return;
        XSetInputFocus(dpy, win, RevertToParent, CurrentTime);
        nanosleep(&ts, NULL);
    }
    die("cannot grab focus");
}

static void grab_keyboard(void) {
    struct timespec ts = {.tv_sec = 0, .tv_nsec = 1000000};
    int i;

    /* try to grab keyboard, we may have to wait for another process to ungrab */
    for (i = 0; i < 1000; i++) {
        if (XGrabKeyboard(dpy, DefaultRootWindow(dpy), True, GrabModeAsync,
                          GrabModeAsync, CurrentTime) == GrabSuccess)
            return;
        nanosleep(&ts, NULL);
    }
    die("cannot grab keyboard");
}

static void create_crtc_windows(XRRScreenResources *sres) {
    XClassHint ch = {"drandr", "drandr"};
    XWindowAttributes wa;
    XSetWindowAttributes swa;
   // Drw *crtc_drw;
    XRRCrtcInfo *crtc_info;
    XRROutputInfo *output_info;
    CrtcWindow *crtc_win;
    int i, j, o, x, y, w, h;

    swa.override_redirect = True;
    swa.background_pixel = scheme[SchemeNorm][ColBg].pixel;
    swa.event_mask = ExposureMask;


    w = 300;

    for (i = 0; i < sres->ncrtc; i++) {
        crtc_info = XRRGetCrtcInfo(dpy, sres, sres->crtcs[i]);
        crtc_win = NULL;

        if (crtc_info->mode == None) {
            XRRFreeCrtcInfo(crtc_info);
            continue;
        };

        for (j = 0; j < n_crtc_wins; j++) {
            if (crtc_wins[j].ctrc == sres->crtcs[i]) {
                crtc_win = &crtc_wins[j];
                break;
            }
        }
        if (crtc_win) {
            XDestroyWindow(dpy, crtc_win->win);
            crtc_win->win = None;
        } else {
            n_crtc_wins++;
            crtc_wins = realloc(crtc_wins, n_crtc_wins * sizeof(CrtcWindow));
            crtc_win = &crtc_wins[n_crtc_wins-1];
            crtc_win->ctrc = sres->crtcs[i];
        }

        x = crtc_info->x + 20 + (w+20)*i;
        y = crtc_info->y + 100;
        h = bh * (crtc_info->noutput + 3);

        crtc_win->win = XCreateWindow(dpy, parentWin, x, y, w, h, 0,
                            CopyFromParent, CopyFromParent, CopyFromParent,
                            CWOverrideRedirect | CWBackPixel | CWEventMask, &swa);
        XSetClassHint(dpy, win, &ch);
        if (!XGetWindowAttributes(dpy, parentWin, &wa))
            die("could not get embedding window attributes: 0x%lx",
                parentWin);

        drw_setscheme(drw, scheme[SchemeSel]);

        for (o = 0; o < crtc_info->noutput; o++) {
            output_info = XRRGetOutputInfo(dpy, sres, crtc_info->outputs[o]);
            drw_text(drw, 0, bh*o, w, bh, lrpad/2, output_info->name, 0);
            XRRFreeOutputInfo(output_info);
        }
        drw_setscheme(drw, scheme[SchemeNorm]);

        snprintf(buf, sizeof buf, "%dx%d", crtc_info->width, crtc_info->height);
        drw_text(drw, 0, bh*crtc_info->noutput, w, bh, lrpad/2, buf, 0);

        XMapRaised(dpy, crtc_win->win);
        drw_map(drw, crtc_win->win, 0, 0, w, h);
        XMapRaised(dpy, crtc_win->win);
        XSync(dpy, False);

        XRRFreeCrtcInfo(crtc_info);
    }
}

static void setup(void) {
    int x, y, i, j;
    unsigned int du;
    XSetWindowAttributes swa;
    XIM xim;
    Window w, dw, *dws;
    XWindowAttributes wa;
    XClassHint ch = {"drandr", "drandr"};
#ifdef XINERAMA
    XineramaScreenInfo *info;
    Window pw;
    int a, di, n, area = 0;
#endif
    OutputConnection *ocun;

    /* init appearance */
    for (j = 0; j < SchemeLast; j++) {
        scheme[j] = drw_scm_create(drw, colors[j], 2);
    }

    /* calculate menu geometry */
    bh = (int) drw->fonts->h + 2;
    lrpad = (int) drw->fonts->h;
    mh = height;
#ifdef XINERAMA
    i = 0;
    if (parentWin == root && (info = XineramaQueryScreens(dpy, &n))) {
        XGetInputFocus(dpy, &w, &di);
        if (mon >= 0 && mon < n)
            i = mon;
        else if (w != root && w != PointerRoot && w != None) {
            /* find top-level window containing current input focus */
            do {
                if (XQueryTree(dpy, (pw = w), &dw, &w, &dws, &du) && dws)
                    XFree(dws);
            } while (w != root && w != pw);
            /* find xinerama screen with which the window intersects most */
            if (XGetWindowAttributes(dpy, pw, &wa))
                for (j = 0; j < n; j++)
                    if ((a = INTERSECT(wa.x, wa.y, wa.width, wa.height, info[j])) > area) {
                        area = a;
                        i = j;
                    }
        }
        /* no focused window is on screen, so use pointer location instead */
        if (mon < 0 && !area && XQueryPointer(dpy, root, &dw, &dw, &x, &y, &di, &di, &du))
            for (i = 0; i < n; i++)
                if (INTERSECT(x, y, 1, 1, info[i]))
                    break;

        mw = MIN(MAX(width, 100), info[i].width);
        x = info[i].x_org + ((info[i].width - mw) / 2);
        y = info[i].y_org + ((info[i].height - mh) / 2);
        XFree(info);
    } else
#endif
    {
        if (!XGetWindowAttributes(dpy, parentWin, &wa))
            die("could not get embedding window attributes: 0x%lx",
                parentWin);
        mw = MIN(MAX(width, 100), wa.width);
        x = (wa.width - mw) / 2;
        y = (wa.height - mh) / 2;
    }

    /* create menu window */
    swa.override_redirect = True;
    swa.background_pixel = scheme[SchemeNorm][ColBg].pixel;
    swa.event_mask = ExposureMask | KeyPressMask | VisibilityChangeMask
            | ButtonPressMask | ButtonReleaseMask | PointerMotionMask;
    win = XCreateWindow(dpy, parentWin, x, y, mw, mh, 0,
                        CopyFromParent, CopyFromParent, CopyFromParent,
                        CWOverrideRedirect | CWBackPixel | CWEventMask, &swa);
    XSetClassHint(dpy, win, &ch);


    /* input methods */
    if ((xim = XOpenIM(dpy, NULL, NULL, NULL)) == NULL)
        die("XOpenIM failed: could not open input device");

    xic = XCreateIC(xim, XNInputStyle, XIMPreeditNothing | XIMStatusNothing,
                    XNClientWindow, win, XNFocusWindow, win, NULL);

    XMapRaised(dpy, win);

    grab_focus();
    grab_keyboard();

    XRRSelectInput(dpy, root, RRScreenChangeNotifyMask | RRCrtcChangeNotifyMask | RROutputChangeNotifyMask | RROutputPropertyNotifyMask);
    XSync(dpy, False);
    get_outputs();

    for (ocun = head; ocun; ocun = ocun->next) {
        if (ocun->crtc_info
                && x >= ocun->crtc_info->x && x <= ocun->crtc_info->x + ocun->crtc_info->width
                && y >= ocun->crtc_info->y && y <= ocun->crtc_info->y + ocun->crtc_info->height) {
            win_output = ocun->output;
        }
    }
    update_canvas();

    draw();
}

static void
usage(void) {
    fputs("usage: drandr [-v]\n", stderr);
    exit(1);
}

int
main(int argc, char *argv[]) {
    XWindowAttributes wa;
    int i;
    struct sigaction sa;

    for (i = 1; i < argc; i++) {

        /* these options take no arguments */
        if (!strcmp(argv[i], "-v")) {      /* prints version information */
            puts("daudio-"
                 VERSION);
            exit(0);
        } else if (i + 1 == argc)
            usage();
            /* these options take one argument */
        else if (!strcmp(argv[i], "-m"))
            mon = (int) strtol(argv[++i], NULL, 10);
        else if (!strcmp(argv[i], "-fn"))  /* font or font set */
            fonts[0] = argv[++i];
        else if (!strcmp(argv[i], "-nf"))  /* normal foreground color */
            colors[SchemeNorm][ColFg] = argv[++i];
        else if (!strcmp(argv[i], "-nb"))  /* normal background color */
            colors[SchemeNorm][ColBg] = argv[++i];
        else if (!strcmp(argv[i], "-sf"))  /* selected foreground color */
            colors[SchemeSel][ColFg] = argv[++i];
        else if (!strcmp(argv[i], "-sb"))  /* selected background color */
            colors[SchemeSel][ColBg] = argv[++i];

        else
            usage();
    }

    int e = atexit(cleanup);
    if (e != 0) {
        die("cannot set exit function");
    }

    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = exit;
    sigaction(SIGINT, &sa, NULL);


    if (!setlocale(LC_CTYPE, "") || !XSupportsLocale())
        fputs("warning: no locale support\n", stderr);
    if (!(dpy = XOpenDisplay(NULL)))
        die("cannot open display");
    screen = DefaultScreen(dpy);
    root = RootWindow(dpy, screen);

    parentWin = root;
    if (!XGetWindowAttributes(dpy, parentWin, &wa))
        die("could not get embedding window attributes: 0x%lx",
            parentWin);
    drw = drw_create(dpy, screen, root, wa.width, wa.height);
    if (!drw_fontset_create(drw, fonts, LENGTH(fonts)))
        die("no fonts could be loaded.");

    setup();
    run();

    return 1; /* unreachable */
}

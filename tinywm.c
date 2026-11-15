/* TinyWM is written by Nick Welch <nick@incise.org> in 2005 & 2011.
 *
 * This software is in the public domain
 * and is provided AS IS, with NO WARRANTY. */

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/Xproto.h>
#include <X11/Xresource.h>
#include <stdio.h>
#include <stdlib.h>

#define MAX(a, b) ((a) > (b) ? (a) : (b))

/* ---- look & feel ---- */
#define PAD       4      /* thickness of the colored border (px) */
#define OUTLINE   1      /* thin outer edge line (px)            */
#define MIN_SIZE  32     /* smallest client size when resizing   */

/* Defaults, used when .Xresources doesn't set a value */
#define DEF_ACTIVE_BG    "#5294e2"
#define DEF_ACTIVE_EDGE  "#1a3a6b"
#define DEF_INACTIVE_BG  "#3c3f41"
#define DEF_INACTIVE_EDGE "#1e1e1e"

typedef struct Client {
    Window client, frame;
    struct Client *next;
} Client;

enum { DRAG_NONE, DRAG_MOVE, DRAG_RESIZE };

static Display *dpy;
static Window root;
static Client *clients, *focused;
static unsigned long col_focus_bg, col_focus_edge, col_blur_bg, col_blur_edge;

/* drag state */
static int drag_mode = DRAG_NONE;
static Window drag_frame = None;
static XButtonEvent start;
static XWindowAttributes attr;
static int drag_left, drag_top;     /* which corner the resize is anchored away from */

static int OnXError(Display *d, XErrorEvent *e)
{
    (void)d;
    if (e->error_code == BadAccess && e->request_code == X_ChangeWindowAttributes) {
        fprintf(stderr, "Another WM is already running\n");
        exit(1);
    }
    if (e->error_code == BadWindow)     /* windows vanish all the time; ignore */
        return 0;
    fprintf(stderr, "XError: code=%d request=%d resource=%lu\n",
            e->error_code, e->request_code, e->resourceid);
    return 0;
}

/* ---- X resources ---- */

static XrmDatabase load_resources(void)
{
    XrmDatabase db = NULL;
    char *rm = XResourceManagerString(dpy);     /* what `xrdb` loaded */

    if (rm) {
        db = XrmGetStringDatabase(rm);
    } else {
        /* xrdb hasn't been run; try reading ~/.Xresources directly */
        const char *home = getenv("HOME");
        if (home) {
            char path[1024];
            snprintf(path, sizeof path, "%s/.Xresources", home);
            db = XrmGetFileDatabase(path);
        }
    }
    return db;
}

static const char *res_get(XrmDatabase db, const char *name, const char *cls)
{
    XrmValue v;
    char *type;
    if (db && XrmGetResource(db, name, cls, &type, &v) && v.addr && *v.addr)
        return v.addr;
    return NULL;
}

static unsigned long alloc_color(const char *name, unsigned long fallback)
{
    XColor c;
    Colormap cmap = DefaultColormap(dpy, DefaultScreen(dpy));
    if (name && XParseColor(dpy, cmap, name, &c) && XAllocColor(dpy, cmap, &c))
        return c.pixel;
    return fallback;
}

/* Try the user's value first; if it's missing or invalid, use the default. */
static unsigned long res_color(XrmDatabase db, const char *name, const char *cls,
                               const char *def, unsigned long fallback)
{
    XColor c;
    Colormap cmap = DefaultColormap(dpy, DefaultScreen(dpy));
    const char *v = res_get(db, name, cls);

    if (v && XParseColor(dpy, cmap, v, &c) && XAllocColor(dpy, cmap, &c))
        return c.pixel;
    if (v)
        fprintf(stderr, "tinywm: bad color '%s' for %s, using default\n", v, name);
    return alloc_color(def, fallback);
}

static void load_colors(void)
{
    unsigned long black = BlackPixel(dpy, DefaultScreen(dpy));
    unsigned long white = WhitePixel(dpy, DefaultScreen(dpy));
    XrmDatabase db;

    XrmInitialize();
    db = load_resources();

    col_focus_bg   = res_color(db, "tinywm.activeColor",   "TinyWM.ActiveColor",   DEF_ACTIVE_BG,     white);
    col_focus_edge = res_color(db, "tinywm.activeEdge",    "TinyWM.ActiveEdge",    DEF_ACTIVE_EDGE,   black);
    col_blur_bg    = res_color(db, "tinywm.inactiveColor", "TinyWM.InactiveColor", DEF_INACTIVE_BG,    black);
    col_blur_edge  = res_color(db, "tinywm.inactiveEdge",  "TinyWM.InactiveEdge",  DEF_INACTIVE_EDGE, black);

    if (db) XrmDestroyDatabase(db);
}

/* ---- client bookkeeping ---- */

static Client *find_client(Window w)
{
    for (Client *c = clients; c; c = c->next)
        if (c->client == w) return c;
    return NULL;
}

static Client *find_frame(Window w)
{
    for (Client *c = clients; c; c = c->next)
        if (c->frame == w) return c;
    return NULL;
}

static void set_color(Client *c, int active)
{
    XSetWindowBackground(dpy, c->frame, active ? col_focus_bg : col_blur_bg);
    XSetWindowBorder(dpy, c->frame, active ? col_focus_edge : col_blur_edge);
    XClearWindow(dpy, c->frame);
}

static void focus_client(Client *c)
{
    if (focused && focused != c) set_color(focused, 0);
    focused = c;
    set_color(c, 1);
    XRaiseWindow(dpy, c->frame);
    XSetInputFocus(dpy, c->client, RevertToPointerRoot, CurrentTime);
}

/* Focus the first managed, viewable window found in stacking order.
 * top_first = 1: from the top (most recent); 0: from the bottom (for cycling). */
static void focus_by_stack(int top_first)
{
    Window r, p, *kids;
    unsigned int n;

    if (!XQueryTree(dpy, root, &r, &p, &kids, &n)) return;
    for (unsigned int k = 0; k < n; k++) {
        Window w = kids[top_first ? n - 1 - k : k];
        Client *c = find_frame(w);
        XWindowAttributes wa;
        if (c && XGetWindowAttributes(dpy, w, &wa) && wa.map_state == IsViewable) {
            focus_client(c);
            break;
        }
    }
    if (kids) XFree(kids);
}

/* Tell the client where it really is (ICCCM: needed because it's reparented). */
static void send_configure(Client *c)
{
    XWindowAttributes fa, ca;
    XConfigureEvent ce = {0};

    if (!XGetWindowAttributes(dpy, c->frame, &fa) ||
        !XGetWindowAttributes(dpy, c->client, &ca))
        return;

    ce.type              = ConfigureNotify;
    ce.display           = dpy;
    ce.event             = c->client;
    ce.window            = c->client;
    ce.x                 = fa.x + fa.border_width + PAD;
    ce.y                 = fa.y + fa.border_width + PAD;
    ce.width             = ca.width;
    ce.height            = ca.height;
    ce.border_width      = 0;
    ce.above             = None;
    ce.override_redirect = False;
    XSendEvent(dpy, c->client, False, StructureNotifyMask, (XEvent *)&ce);
}

/* Wrap a client window in a new frame and map both. */
static Client *frame_window(Window w)
{
    XWindowAttributes a;
    Client *c;

    if (!XGetWindowAttributes(dpy, w, &a) || a.override_redirect) return NULL;
    if (!(c = calloc(1, sizeof *c))) return NULL;

    c->client = w;
    c->frame = XCreateSimpleWindow(dpy, root,
                   MAX(0, a.x - PAD - OUTLINE), MAX(0, a.y - PAD - OUTLINE),
                   a.width + 2 * PAD, a.height + 2 * PAD,
                   OUTLINE, col_blur_edge, col_blur_bg);

    XSelectInput(dpy, c->frame, SubstructureRedirectMask | SubstructureNotifyMask);

    /* Click-to-focus; the press is frozen until we replay or keep it */
    XGrabButton(dpy, AnyButton, AnyModifier, c->frame, False,
                ButtonPressMask | ButtonReleaseMask | PointerMotionMask,
                GrabModeSync, GrabModeAsync, None, None);

    XSetWindowBorderWidth(dpy, w, 0);
    XAddToSaveSet(dpy, w);
    XReparentWindow(dpy, w, c->frame, PAD, PAD);
    XMapWindow(dpy, c->frame);
    XMapWindow(dpy, w);

    c->next = clients;
    clients = c;
    return c;
}

static void remove_client(Client *c, int reparent)
{
    int was_focused = (c == focused);

    if (drag_frame == c->frame) { drag_mode = DRAG_NONE; drag_frame = None; }

    if (reparent) {
        XWindowAttributes fa;
        int x = 0, y = 0;
        if (XGetWindowAttributes(dpy, c->frame, &fa)) {
            x = fa.x + fa.border_width + PAD;
            y = fa.y + fa.border_width + PAD;
        }
        XReparentWindow(dpy, c->client, root, x, y);
        XRemoveFromSaveSet(dpy, c->client);
    }
    XDestroyWindow(dpy, c->frame);

    for (Client **pp = &clients; *pp; pp = &(*pp)->next)
        if (*pp == c) { *pp = c->next; break; }

    if (was_focused) {
        focused = NULL;
        focus_by_stack(1);
    }
    free(c);
}

static void close_window(Window w)
{
    Atom wm_protocols = XInternAtom(dpy, "WM_PROTOCOLS", False);
    Atom wm_delete    = XInternAtom(dpy, "WM_DELETE_WINDOW", False);
    Atom *protos = NULL;
    int n = 0, supported = 0;

    if (XGetWMProtocols(dpy, w, &protos, &n)) {
        for (int i = 0; i < n; i++)
            if (protos[i] == wm_delete) supported = 1;
        XFree(protos);
    }

    if (supported) {
        XEvent ev = {0};
        ev.xclient.type         = ClientMessage;
        ev.xclient.window       = w;
        ev.xclient.message_type = wm_protocols;
        ev.xclient.format       = 32;
        ev.xclient.data.l[0]    = wm_delete;
        ev.xclient.data.l[1]    = CurrentTime;
        XSendEvent(dpy, w, False, NoEventMask, &ev);
    } else {
        XKillClient(dpy, w);
    }
}

/* Start a move or resize. For resizes, remember which corner was grabbed
 * so the opposite corner stays fixed. */
static void begin_drag(XButtonEvent *e, Window frame, int mode)
{
    if (!XGetWindowAttributes(dpy, frame, &attr)) return;

    start      = *e;
    drag_frame = frame;
    drag_mode  = mode;

    int total_w = attr.width  + 2 * attr.border_width;
    int total_h = attr.height + 2 * attr.border_width;
    drag_left = (e->x_root - attr.x) < total_w / 2;
    drag_top  = (e->y_root - attr.y) < total_h / 2;
}

int main(void)
{
    XEvent ev;

    if (!(dpy = XOpenDisplay(NULL))) return 1;
    root = DefaultRootWindow(dpy);

    XSetErrorHandler(OnXError);
    XSelectInput(dpy, root, SubstructureRedirectMask | SubstructureNotifyMask);
    XSync(dpy, False);

    load_colors();

    KeyCode key_f1  = XKeysymToKeycode(dpy, XStringToKeysym("F1"));
    KeyCode key_f4  = XKeysymToKeycode(dpy, XStringToKeysym("F4"));
    KeyCode key_tab = XKeysymToKeycode(dpy, XStringToKeysym("Tab"));

    XGrabKey(dpy, key_f1,  Mod1Mask, root, True, GrabModeAsync, GrabModeAsync);
    XGrabKey(dpy, key_f4,  Mod1Mask, root, True, GrabModeAsync, GrabModeAsync);
    XGrabKey(dpy, key_tab, Mod1Mask, root, True, GrabModeAsync, GrabModeAsync);
    XGrabButton(dpy, 1, Mod1Mask, root, True,
                ButtonPressMask | ButtonReleaseMask | PointerMotionMask,
                GrabModeAsync, GrabModeAsync, None, None);
    XGrabButton(dpy, 3, Mod1Mask, root, True,
                ButtonPressMask | ButtonReleaseMask | PointerMotionMask,
                GrabModeAsync, GrabModeAsync, None, None);

    /* Adopt windows that already exist */
    XGrabServer(dpy);
    {
        Window r, p, *kids;
        unsigned int n;
        if (XQueryTree(dpy, root, &r, &p, &kids, &n)) {
            for (unsigned int i = 0; i < n; i++) {
                XWindowAttributes a;
                if (XGetWindowAttributes(dpy, kids[i], &a) &&
                    !a.override_redirect && a.map_state == IsViewable)
                    frame_window(kids[i]);
            }
            if (kids) XFree(kids);
        }
    }
    XUngrabServer(dpy);
    focus_by_stack(1);

    for (;;) {
        XNextEvent(dpy, &ev);

        switch (ev.type) {

        case MapRequest: {
            Window w = ev.xmaprequest.window;
            Client *c = find_client(w);
            if (!c) c = frame_window(w);
            if (c) {
                XMapWindow(dpy, c->frame);
                XMapWindow(dpy, w);
                focus_client(c);
            } else {
                XMapWindow(dpy, w);
            }
            break;
        }

        case ConfigureRequest: {
            XConfigureRequestEvent *e = &ev.xconfigurerequest;
            Client *c = find_client(e->window);

            if (c) {
                XWindowAttributes fa, ca;
                if (!XGetWindowAttributes(dpy, c->frame, &fa) ||
                    !XGetWindowAttributes(dpy, c->client, &ca))
                    break;
                int cw = (e->value_mask & CWWidth)  ? MAX(1, e->width)  : ca.width;
                int ch = (e->value_mask & CWHeight) ? MAX(1, e->height) : ca.height;
                int fx = (e->value_mask & CWX) ? MAX(0, e->x - PAD - fa.border_width) : fa.x;
                int fy = (e->value_mask & CWY) ? MAX(0, e->y - PAD - fa.border_width) : fa.y;

                XMoveResizeWindow(dpy, c->frame, fx, fy, cw + 2 * PAD, ch + 2 * PAD);
                XResizeWindow(dpy, c->client, cw, ch);
                if ((e->value_mask & CWStackMode) && e->detail == Above)
                    XRaiseWindow(dpy, c->frame);
                send_configure(c);
            } else {
                XWindowChanges wc;
                wc.x = e->x;  wc.y = e->y;
                wc.width = e->width;  wc.height = e->height;
                wc.border_width = e->border_width;
                wc.sibling = e->above;
                wc.stack_mode = e->detail;
                XConfigureWindow(dpy, e->window, e->value_mask, &wc);
            }
            break;
        }

        case UnmapNotify: {
            if (ev.xunmap.event == root) break;
            Client *c = find_client(ev.xunmap.window);
            if (c) remove_client(c, 1);
            break;
        }

        case DestroyNotify: {
            Client *c = find_client(ev.xdestroywindow.window);
            if (c) remove_client(c, 0);
            break;
        }

        case KeyPress: {
            KeyCode kc = ev.xkey.keycode;
            Client *under = find_frame(ev.xkey.subwindow);

            if (kc == key_f1) {
                if (under) focus_client(under);
            }
            else if (kc == key_f4) {
                Client *t = focused ? focused : under;
                if (t) close_window(t->client);
            }
            else if (kc == key_tab) {
                focus_by_stack(0);
            }
            break;
        }

        case ButtonPress: {
            Client *c;
            if (ev.xbutton.window == root) {
                /* Alt+drag anywhere on a window: left = move, right = resize */
                if ((c = find_frame(ev.xbutton.subwindow))) {
                    focus_client(c);
                    begin_drag(&ev.xbutton, c->frame,
                               ev.xbutton.button == 1 ? DRAG_MOVE : DRAG_RESIZE);
                }
            } else if ((c = find_frame(ev.xbutton.window))) {
                focus_client(c);
                if (ev.xbutton.subwindow == None &&
                    (ev.xbutton.button == 1 || ev.xbutton.button == 3)) {
                    /* On the colored border: left = resize, right = move */
                    begin_drag(&ev.xbutton, c->frame,
                               ev.xbutton.button == 1 ? DRAG_RESIZE : DRAG_MOVE);
                    XAllowEvents(dpy, AsyncPointer, CurrentTime);
                } else {
                    /* client area: hand the click on to the application */
                    XAllowEvents(dpy, ReplayPointer, CurrentTime);
                }
            }
            break;
        }

        case MotionNotify: {
            if (drag_mode == DRAG_NONE) break;
            while (XCheckTypedEvent(dpy, MotionNotify, &ev))
                ;
            Client *c = find_frame(drag_frame);
            if (!c) { drag_mode = DRAG_NONE; break; }

            int xdiff = ev.xmotion.x_root - start.x_root;
            int ydiff = ev.xmotion.y_root - start.y_root;

            if (drag_mode == DRAG_MOVE) {
                XMoveWindow(dpy, c->frame, attr.x + xdiff, attr.y + ydiff);
            } else {
                int cw0 = attr.width  - 2 * PAD;
                int ch0 = attr.height - 2 * PAD;
                int cw = MAX(MIN_SIZE, cw0 + (drag_left ? -xdiff : xdiff));
                int ch = MAX(MIN_SIZE, ch0 + (drag_top  ? -ydiff : ydiff));
                /* grabbing the left/top side moves the origin so the far side stays put */
                int fx = attr.x + (drag_left ? cw0 - cw : 0);
                int fy = attr.y + (drag_top  ? ch0 - ch : 0);

                XMoveResizeWindow(dpy, c->frame, fx, fy, cw + 2 * PAD, ch + 2 * PAD);
                XResizeWindow(dpy, c->client, cw, ch);
            }
            send_configure(c);
            break;
        }

        case ButtonRelease:
            drag_mode = DRAG_NONE;
            drag_frame = None;
            break;
        }
    }
}

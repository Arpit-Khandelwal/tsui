#include <stdlib.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <dlfcn.h>
#include <X11/Xlib.h>
#include <X11/Xatom.h>

// sendStatus is a function from the Go side. Must only be called once per handle.
extern void sendStatus(uintptr_t statusHandle, int value);

// libX11 is loaded at runtime instead of being linked, so that tsui can still start on
// systems without X11 installed (e.g. headless servers). Only the clipboard needs it.
static Display *(*pXOpenDisplay)(const char *);
static int (*pXCloseDisplay)(Display *);
static Window (*pXDefaultRootWindow)(Display *);
static Window (*pXCreateSimpleWindow)(Display *, Window, int, int, unsigned int, unsigned int,
                                      unsigned int, unsigned long, unsigned long);
static Atom (*pXInternAtom)(Display *, const char *, Bool);
static int (*pXSetSelectionOwner)(Display *, Atom, Window, Time);
static Window (*pXGetSelectionOwner)(Display *, Atom);
static int (*pXNextEvent)(Display *, XEvent *);
static int (*pXChangeProperty)(Display *, Window, Atom, Atom, int, int, const unsigned char *, int);
static Status (*pXSendEvent)(Display *, Window, Bool, long, XEvent *);

// Loads libX11 and resolves the functions we need. Returns true on success. Callers are
// serialized by the lock on the Go side.
static bool loadX11(void)
{
    static bool loaded = false;
    if (loaded)
    {
        return true;
    }

    void *lib = dlopen("libX11.so.6", RTLD_LAZY | RTLD_LOCAL);
    if (lib == NULL)
    {
        lib = dlopen("libX11.so", RTLD_LAZY | RTLD_LOCAL);
    }
    if (lib == NULL)
    {
        return false;
    }

    if ((pXOpenDisplay = dlsym(lib, "XOpenDisplay")) == NULL ||
        (pXCloseDisplay = dlsym(lib, "XCloseDisplay")) == NULL ||
        (pXDefaultRootWindow = dlsym(lib, "XDefaultRootWindow")) == NULL ||
        (pXCreateSimpleWindow = dlsym(lib, "XCreateSimpleWindow")) == NULL ||
        (pXInternAtom = dlsym(lib, "XInternAtom")) == NULL ||
        (pXSetSelectionOwner = dlsym(lib, "XSetSelectionOwner")) == NULL ||
        (pXGetSelectionOwner = dlsym(lib, "XGetSelectionOwner")) == NULL ||
        (pXNextEvent = dlsym(lib, "XNextEvent")) == NULL ||
        (pXChangeProperty = dlsym(lib, "XChangeProperty")) == NULL ||
        (pXSendEvent = dlsym(lib, "XSendEvent")) == NULL)
    {
        dlclose(lib);
        return false;
    }

    loaded = true;
    return true;
}

// Writes `n` bytes from `buf` to the clipboard. Sends an int to the passed `statusHandle`
// (which must be a handle to a Go channel) exactly once: negative for failure, 0 for
// success copying.
//
// X handles clipboards very weirdly and the content only persists as long as the owning
// process continues to run. Therefore, this needs to continue running in a thread until
// the ownership is taken by another app, at which point it will return.
//
// @see https://www.uninformativ.de/blog/postings/2017-04-02/0/POSTING-en.html
void writeString(unsigned char *buf, size_t n, uintptr_t statusHandle)
{
    if (!loadX11())
    {
        // libX11 isn't available.
        sendStatus(statusHandle, -3);
        return;
    }

    // Attempt to open an X display.
    Display *display = NULL;
    for (int i = 0; i < 42; i++)
    {
        display = pXOpenDisplay(0);
        if (display == NULL)
        {
            continue;
        }
        break;
    }
    if (display == NULL)
    {
        // Couldn't open a display.
        sendStatus(statusHandle, -1);
        return;
    }

    // Create an invisible window to use as an agent for clipboard operations.
    Window window = pXCreateSimpleWindow(display, pXDefaultRootWindow(display), 0, 0, 1, 1, 0, 0, 0);

    Atom clipboardAtom = pXInternAtom(display, "CLIPBOARD", false);
    Atom utf8Atom = pXInternAtom(display, "UTF8_STRING", false);
    Atom targetsAtom = pXInternAtom(display, "TARGETS", false);

    // Set our window as the owner of the clipboard.
    pXSetSelectionOwner(display, clipboardAtom, window, CurrentTime);
    if (pXGetSelectionOwner(display, clipboardAtom) != window)
    {
        // We couldn't set ourself as the owner.
        pXCloseDisplay(display);
        sendStatus(statusHandle, -2);
        return;
    }

    sendStatus(statusHandle, 0);

    XEvent event;
    while (true)
    {
        pXNextEvent(display, &event);

        switch (event.type)
        {
        // We lost ownership of the clipboard, which means we can exit this process.
        case SelectionClear:
            return;

        // Someone wants to paste our data.
        case SelectionRequest:
            XSelectionRequestEvent *xsr = &event.xselectionrequest;

            if (xsr->selection != clipboardAtom)
            {
                // Not for us.
                break;
            }

            XSelectionEvent ev = {0};
            int error = 0;

            ev.type = SelectionNotify;
            ev.display = xsr->display;
            ev.requestor = xsr->requestor;
            ev.selection = xsr->selection;
            ev.time = xsr->time;
            ev.target = xsr->target;
            ev.property = xsr->property;

            if (ev.target == utf8Atom)
            {
                // Reply with our string.
                error = pXChangeProperty(ev.display, ev.requestor, ev.property,
                                         utf8Atom, 8, PropModeReplace,
                                         buf, n);
            }
            else if (ev.target == targetsAtom)
            {
                // Reply with the targets we support (only UTF-8).
                Atom targets[] = {utf8Atom};
                error = pXChangeProperty(ev.display, ev.requestor, ev.property,
                                         XA_ATOM, 32, PropModeReplace,
                                         (unsigned char *)&targets, sizeof(targets) / sizeof(Atom));
            }
            else
            {
                // Deny the request.
                ev.property = None;
            }

            // Send the event if XChangeProperty either succeeded or didn't run.
            if ((error & 2) == 0)
            {
                pXSendEvent(display, ev.requestor, 0, 0, (XEvent *)&ev);
            }

            break;
        }
    }
}

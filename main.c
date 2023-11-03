#include <Imlib2.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define auto __auto_type

#define LENGTH(arr) (sizeof(arr) / sizeof((arr)[0]))

enum { DEFAULT_WIDTH = 800, DEFAULT_HEIGHT = 600, PAN_AMOUNT = 50 };

static float const ZOOM_LEVELS[] = {0.125f, 0.25f, 0.75f, 1.0f,  1.5f,
                                    2.0f,   4.0f,  8.0f,  12.0f, 16.0f};

static float smaller_zoom(float zoom) {
    for (size_t i = LENGTH(ZOOM_LEVELS); i--;) {
        if (ZOOM_LEVELS[i] < zoom) {
            return ZOOM_LEVELS[i];
        }
    }
    return zoom;
}

static float larger_zoom(float zoom) {
    for (size_t i = 0; i < LENGTH(ZOOM_LEVELS); ++i) {
        if (ZOOM_LEVELS[i] > zoom) {
            return ZOOM_LEVELS[i];
        }
    }
    return zoom;
}

typedef struct {
    Display *display;
    Window window;
    int window_width, window_height;
    Atom atom_wm_delete_window;
    float zoom_level;
    struct {
        int x, y;
    } pan;
    bool dirty;
    bool quit;
} App;

static App app_new(char const *image_path) {
    auto display = XOpenDisplay(NULL);
    assert(display && "failed to open display");

    auto screen = DefaultScreen(display);
    auto root = RootWindow(display, screen);

    auto window = XCreateSimpleWindow(
        display, root, 0, 0, DEFAULT_WIDTH, DEFAULT_HEIGHT, 0, None, None
    );

    XStoreName(display, window, "iv");
    XSelectInput(
        display, window, ExposureMask | KeyPressMask | StructureNotifyMask
    );
    XSetWindowBackgroundPixmap(display, window, None);
    XMapWindow(display, window);

    auto image = imlib_load_image(image_path);
    assert(image && "failed to open image");
    imlib_context_set_image(image);
    imlib_context_set_display(display);
    imlib_context_set_visual(DefaultVisual(display, screen));
    imlib_context_set_drawable(window);

    auto atom_wm_delete_window = XInternAtom(display, "WM_DELETE_WINDOW", 0);
    XSetWMProtocols(display, window, &atom_wm_delete_window, 1);

    return (App){
        .display = display,
        .window = window,
        .window_width = DEFAULT_WIDTH,
        .window_height = DEFAULT_HEIGHT,
        .atom_wm_delete_window = atom_wm_delete_window,
        .zoom_level = 1.0f,
        .pan =
            {
                .x = 0,
                .y = 0,
            },
        .dirty = false,
        .quit = false,
    };
}

static void render(App const *app, Imlib_Updates updates) {
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
    imlib_updates_get_coordinates(updates, &x, &y, &width, &height);
    auto source_width = (int)((float)width / app->zoom_level);
    auto source_height = (int)((float)height / app->zoom_level);
    imlib_render_image_part_on_drawable_at_size(
        x + app->pan.x, y + app->pan.y, source_width, source_height, x, y,
        width, height
    );
}

static void render_all_updates(App *app, Imlib_Updates updates) {
    if (app->dirty) {
        app->dirty = false;
        imlib_updates_free(updates);
        updates = imlib_update_append_rect(
            NULL, 0, 0, app->window_width, app->window_height
        );
    } else {
        updates = imlib_updates_merge_for_rendering(
            updates, app->window_width, app->window_height
        );
    }
    for (auto update = updates; update;
         update = imlib_updates_get_next(update)) {
        render(app, update);
    }
    imlib_updates_free(updates);
}

static void set_zoom_level(App *app, float zoom) {
    if (app->zoom_level != zoom) {
        app->zoom_level = zoom;
        app->dirty = true;
    }
}

static void set_pan_x(App *app, int x) {
    if (x < 0 || (float)imlib_image_get_width() * app->zoom_level <=
                     (float)app->window_width) {
        return;
    }

    app->pan.x = x;
    app->dirty = true;
}

static void set_pan_y(App *app, int y) {
    if (y < 0 || (float)imlib_image_get_height() * app->zoom_level <=
                     (float)app->window_height) {
        return;
    }

    app->pan.y = y;
    app->dirty = true;
}

static void handle_key_press(App *app, XKeyEvent *event) {
    KeySym key = 0;
    XLookupString(event, NULL, 0, &key, NULL);
    switch (key) {
    case XK_q:
        app->quit = true;
        break;
    case XK_minus:
        set_zoom_level(app, smaller_zoom(app->zoom_level));
        break;
    case XK_plus:
        set_zoom_level(app, larger_zoom(app->zoom_level));
        break;
    case XK_equal:
        set_zoom_level(app, 1.0f);
        break;
    case XK_h:
        set_pan_x(app, app->pan.x - PAN_AMOUNT);
        break;
    case XK_l:
        set_pan_x(app, app->pan.x + PAN_AMOUNT);
        break;
    case XK_k:
        set_pan_y(app, app->pan.y - PAN_AMOUNT);
        break;
    case XK_j:
        set_pan_y(app, app->pan.y + PAN_AMOUNT);
        break;
    default:;
    }
}

static void app_run(App *app) {
    auto updates = imlib_updates_init();

    do {
        XEvent event;
        XNextEvent(app->display, &event);
        switch (event.type) {
        case Expose: {
            auto expose = event.xexpose;
            updates = imlib_update_append_rect(
                updates, expose.x, expose.y, expose.width, expose.height
            );
        } break;
        case KeyPress:
            handle_key_press(app, &event.xkey);
            break;
        case ConfigureNotify: {
            auto size = event.xconfigure;
            app->window_width = size.width;
            app->window_height = size.height;
        } break;
        case ClientMessage:
            if ((Atom)event.xclient.data.l[0] == app->atom_wm_delete_window) {
                app->quit = true;
            }
            break;
        }
    } while (XPending(app->display));

    render_all_updates(app, updates);
}

// lf breaks if it tries to resize really quickly after running a shell command,
// so we wait a bit.
static void sleep_to_not_break_lf(void) {
    auto parent_pid = getppid();
    char cmdline_path[sizeof("/proc/2147483647/cmdline")];
    (void)sprintf(cmdline_path, "/proc/%d/cmdline", parent_pid);
    auto cmdline_file = fopen(cmdline_path, "r");
    char cmdline[sizeof("lf") - 1] = {0};
    (void)fread(cmdline, sizeof(cmdline), 1, cmdline_file);
    (void)fclose(cmdline_file);
    if (memcmp(cmdline, "lf", sizeof(cmdline)) == 0) {
        auto const timespec = (struct timespec){
            .tv_sec = 0,
            .tv_nsec = 100000000,
        };
        nanosleep(&timespec, NULL);
    }
}

static void app_deinit(App const *app) {
    XDestroyWindow(app->display, app->window);
    XCloseDisplay(app->display);
    imlib_free_image();
    sleep_to_not_break_lf();
}

int main(int argc, char *argv[]) {
    auto image_path = argc > 1 ? argv[1] : "image.jpg";

    auto app = app_new(image_path);
    while (!app.quit) {
        app_run(&app);
    }
    app_deinit(&app);
}

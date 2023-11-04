#include <Imlib2.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define auto __auto_type

#define LENGTH(arr) (sizeof(arr) / sizeof((arr)[0]))

enum { DEFAULT_WIDTH = 800, DEFAULT_HEIGHT = 600, PAN_AMOUNT = 5 };

static float const ZOOM_LEVELS[] = {0.125f, 0.25f, 0.75f, 1.0f,  1.5f,
                                    2.0f,   4.0f,  8.0f,  12.0f, 16.0f};

static float smaller_zoom(float level) {
    for (size_t i = LENGTH(ZOOM_LEVELS); i--;) {
        if (ZOOM_LEVELS[i] < level) {
            return ZOOM_LEVELS[i];
        }
    }
    return level;
}

static float larger_zoom(float level) {
    for (size_t i = 0; i < LENGTH(ZOOM_LEVELS); ++i) {
        if (ZOOM_LEVELS[i] > level) {
            return ZOOM_LEVELS[i];
        }
    }
    return level;
}

typedef enum {
    ZoomManual,
} ZoomMode;

typedef struct {
    Display *display;
    Window window;
    GC gc;
    int window_width, window_height;
    Atom atom_wm_delete_window;
    struct {
        int x, y;
    } pan;
    struct {
        float level;
        ZoomMode mode;
    } zoom;
    bool dirty;
    bool quit;
} App;

static App app_new(char const *image_path) {
    auto display = XOpenDisplay(NULL);
    if (!display) {
        (void)fputs("error: failed to open display\n", stderr);
        exit(EXIT_FAILURE);
    }

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
    if (!image) {
        (void)fprintf(stderr, "error: failed to open image %s\n", image_path);
        exit(EXIT_FAILURE);
    }
    imlib_context_set_image(image);
    imlib_context_set_display(display);
    imlib_context_set_visual(DefaultVisual(display, screen));
    imlib_context_set_drawable(window);

    auto atom_wm_delete_window = XInternAtom(display, "WM_DELETE_WINDOW", 0);
    XSetWMProtocols(display, window, &atom_wm_delete_window, 1);

    return (App){
        .display = display,
        .window = window,
        .gc = DefaultGC(display, screen),
        .window_width = DEFAULT_WIDTH,
        .window_height = DEFAULT_HEIGHT,
        .atom_wm_delete_window = atom_wm_delete_window,
        .zoom =
            {
                .level = 1.0f,
                .mode = ZoomManual,
            },
        .pan =
            {
                .x = 0,
                .y = 0,
            },
        .dirty = false,
        .quit = false,
    };
}

static void render_background(
    App const *app, int x, int y, unsigned width, unsigned height
) {
    XFillRectangle(app->display, app->window, app->gc, x, y, width, height);
}

static void
clip_image_top(App const *app, int x, int *y, int width, int *height) {
    if (*y < -app->pan.y) {
        auto background_height =
            *y + *height > -app->pan.y ? -app->pan.y - *y : *height;
        render_background(
            app, x, *y, (unsigned)width, (unsigned)background_height
        );
        *height += app->pan.y + *y;
        *y = -app->pan.y;
    }
}

static void
clip_image_left(App const *app, int *x, int y, int *width, int height) {
    if (*x < -app->pan.x) {
        auto background_width =
            *x + *width > -app->pan.x ? -app->pan.x - *x : *width;
        render_background(
            app, *x, y, (unsigned)background_width, (unsigned)height
        );
        *width += app->pan.x + *x;
        *x = -app->pan.x;
    }
}

static void
clip_image_bottom(App const *app, int x, int y, int width, int *height) {
    auto edge =
        (int)((float)(imlib_image_get_height() - app->pan.y) * app->zoom.level);
    if (edge < app->window_height) {
        auto background_height = y > edge ? *height : y + *height - edge;
        render_background(
            app, x, edge, (unsigned)width, (unsigned)background_height
        );
        *height = edge - y;
    }
}

static void render(App const *app, Imlib_Updates updates) {
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
    imlib_updates_get_coordinates(updates, &x, &y, &width, &height);
    clip_image_top(app, x, &y, width, &height);
    clip_image_left(app, &x, y, &width, height);
    clip_image_bottom(app, x, y, width, &height);
    auto source_width = (int)((float)width / app->zoom.level);
    auto source_height = (int)((float)height / app->zoom.level);
    auto source_x = (int)((float)(x + app->pan.x) / app->zoom.level);
    auto source_y = (int)((float)(y + app->pan.y) / app->zoom.level);
    imlib_render_image_part_on_drawable_at_size(
        source_x, source_y, source_width, source_height, x, y, width, height
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

static void set_zoom_level(App *app, float level) {
    if (app->zoom.level != level) {
        app->zoom.level = level;
        app->dirty = true;
    }
    app->zoom.mode = ZoomManual;
}

static int clamp_pan_x(App const *app, int x) {
    if (x < 0) {
        return 0;
    }
    auto image_width = (int)((float)imlib_image_get_width() * app->zoom.level);
    return x > image_width - app->window_width ? image_width - app->window_width
                                               : x;
}

static int clamp_pan_y(App const *app, int y) {
    if (y < 0) {
        return 0;
    }
    auto image_height =
        (int)((float)imlib_image_get_height() * app->zoom.level);
    return y > image_height - app->window_height
               ? image_height - app->window_height
               : y;
}

static void set_pan_x(App *app, int x) {
    if ((float)imlib_image_get_width() * app->zoom.level <=
        (float)app->window_width) {
        return;
    }

    app->pan.x = clamp_pan_x(app, x);
    app->dirty = true;
}

static void set_pan_y(App *app, int y) {
    if ((float)imlib_image_get_height() * app->zoom.level <=
        (float)app->window_height) {
        return;
    }

    app->pan.y = clamp_pan_y(app, y);
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
        set_zoom_level(app, smaller_zoom(app->zoom.level));
        break;
    case XK_plus:
        set_zoom_level(app, larger_zoom(app->zoom.level));
        break;
    case XK_equal:
        set_zoom_level(app, 1.0f);
        break;
    case XK_h:
        set_pan_x(app, app->pan.x - app->window_width / PAN_AMOUNT);
        break;
    case XK_l:
        set_pan_x(app, app->pan.x + app->window_width / PAN_AMOUNT);
        break;
    case XK_k:
        set_pan_y(app, app->pan.y - app->window_height / PAN_AMOUNT);
        break;
    case XK_j:
        set_pan_y(app, app->pan.y + app->window_height / PAN_AMOUNT);
        break;
    case XK_H:
        set_pan_x(app, 0);
        break;
    case XK_K:
        set_pan_y(app, 0);
        break;
    case XK_a:
        imlib_context_set_anti_alias(imlib_context_get_anti_alias() ? 0 : 1);
        app->dirty = true;
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
    if (argc < 2) {
        (void)fputs("error: no images provided\n", stderr);
        return EXIT_FAILURE;
    }
    auto image_path = argv[1];

    auto app = app_new(image_path);
    while (!app.quit) {
        app_run(&app);
    }
    app_deinit(&app);
}

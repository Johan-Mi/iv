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
    struct {
        int x, y;
    } pan;
    Imlib_Image im;
    struct {
        float level;
        ZoomMode mode;
    } zoom;
} Image;

typedef struct {
    Image *items;
    size_t count;
} Images;

typedef struct {
    Display *display;
    Window window;
    GC gc;
    int window_width, window_height;
    Atom atom_wm_delete_window;
    Image *img;
    Images images;
    bool dirty;
    bool quit;
} App;

static App app_new(Images images) {
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

    imlib_context_set_image(images.items[0].im);
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
        .img = &images.items[0],
        .images = images,
        .dirty = false,
        .quit = false,
    };
}

static void render_background(
    App const *app, int x, int y, unsigned width, unsigned height
) {
    XFillRectangle(app->display, app->window, app->gc, x, y, width, height);
}

static int rendered_image_width(float zoom_level) {
    return (int)((float)imlib_image_get_width() * zoom_level);
}

static int rendered_image_height(float zoom_level) {
    return (int)((float)imlib_image_get_height() * zoom_level);
}

static void
clip_image_top(App const *app, int x, int *y, int width, int *height) {
    auto img = app->img;
    auto edge = (app->window_height - rendered_image_height(img->zoom.level) +
                 img->pan.y) /
                2;
    if (*y < edge) {
        auto background_height = *y + *height > edge ? edge - *y : *height;
        render_background(
            app, x, *y, (unsigned)width, (unsigned)background_height
        );
        *height -= background_height;
        *y += background_height;
    }
}

static void
clip_image_left(App const *app, int *x, int y, int *width, int height) {
    auto img = app->img;
    auto edge = (app->window_width - rendered_image_width(img->zoom.level) +
                 img->pan.x) /
                2;
    if (*x < edge) {
        auto background_width = *x + *width > edge ? edge - *x : *width;
        render_background(
            app, *x, y, (unsigned)background_width, (unsigned)height
        );
        *width -= background_width;
        *x += background_width;
    }
}

static void
clip_image_bottom(App const *app, int x, int y, int width, int *height) {
    auto img = app->img;
    auto edge = (rendered_image_height(img->zoom.level) - img->pan.y +
                 app->window_height) /
                2;
    if (edge < app->window_height) {
        auto background_height = y > edge ? *height : y + *height - edge;
        render_background(
            app, x, edge, (unsigned)width, (unsigned)background_height
        );
        *height = edge - y;
    }
}

static void
clip_image_right(App const *app, int x, int y, int *width, int height) {
    auto img = app->img;
    auto edge = (rendered_image_width(img->zoom.level) - img->pan.x +
                 app->window_width) /
                2;
    if (edge < app->window_width) {
        auto background_width = x > edge ? *width : x + *width - edge;
        render_background(
            app, edge, y, (unsigned)background_width, (unsigned)height
        );
        *width = edge - x;
    }
}

static void render(App const *app, Imlib_Updates updates) {
    auto img = app->img;
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
    imlib_updates_get_coordinates(updates, &x, &y, &width, &height);
    clip_image_top(app, x, &y, width, &height);
    clip_image_left(app, &x, y, &width, height);
    clip_image_bottom(app, x, y, width, &height);
    clip_image_right(app, x, y, &width, height);
    auto source_width = (int)((float)width / img->zoom.level);
    auto source_height = (int)((float)height / img->zoom.level);
    auto source_x =
        (int)(((float)(x + img->pan.x) - (float)app->window_width / 2) /
              img->zoom.level) +
        imlib_image_get_width() / 2;
    auto source_y =
        (int)(((float)(y + img->pan.y) - (float)app->window_height / 2) /
              img->zoom.level) +
        imlib_image_get_height() / 2;
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

static void center_image(App *app) {
    auto img = app->img;
    if (rendered_image_width(img->zoom.level) < app->window_width) {
        img->pan.x = 0;
        app->dirty = true;
    }

    if (rendered_image_height(img->zoom.level) < app->window_height) {
        img->pan.y = 0;
        app->dirty = true;
    }
}

static void set_zoom_level(App *app, float level) {
    auto img = app->img;
    if (img->zoom.level != level) {
        img->zoom.level = level;
        center_image(app);
        app->dirty = true;
    }
    img->zoom.mode = ZoomManual;
}

static int clamp_pan_x(App const *app, int x) {
    auto limit =
        (app->window_width - rendered_image_width(app->img->zoom.level)) / 2;
    return x < limit ? limit : x > -limit ? -limit : x;
}

static int clamp_pan_y(App const *app, int y) {
    auto limit =
        (app->window_height - rendered_image_height(app->img->zoom.level)) / 2;
    return y < limit ? limit : y > -limit ? -limit : y;
}

static void set_pan_x(App *app, int x) {
    if (rendered_image_width(app->img->zoom.level) <= app->window_width) {
        return;
    }

    app->img->pan.x = clamp_pan_x(app, x);
    app->dirty = true;
}

static void set_pan_y(App *app, int y) {
    if (rendered_image_height(app->img->zoom.level) <= app->window_height) {
        return;
    }

    app->img->pan.y = clamp_pan_y(app, y);
    app->dirty = true;
}

static void switch_image(App *app, int offset) {
    auto index = (size_t)(app->img - app->images.items + offset);
    if (index < 0 || index >= app->images.count) {
        return;
    }
    app->img = &app->images.items[index];
    imlib_context_set_image(app->img->im);
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
        set_zoom_level(app, smaller_zoom(app->img->zoom.level));
        break;
    case XK_plus:
        set_zoom_level(app, larger_zoom(app->img->zoom.level));
        break;
    case XK_equal:
        set_zoom_level(app, 1.0f);
        break;
    case XK_h:
        set_pan_x(app, app->img->pan.x - app->window_width / PAN_AMOUNT);
        break;
    case XK_l:
        set_pan_x(app, app->img->pan.x + app->window_width / PAN_AMOUNT);
        break;
    case XK_k:
        set_pan_y(app, app->img->pan.y - app->window_height / PAN_AMOUNT);
        break;
    case XK_j:
        set_pan_y(app, app->img->pan.y + app->window_height / PAN_AMOUNT);
        break;
    case XK_H:
        set_pan_x(
            app,
            (app->window_width - rendered_image_width(app->img->zoom.level)) / 2
        );
        break;
    case XK_K:
        set_pan_y(
            app,
            (app->window_height - rendered_image_height(app->img->zoom.level)) /
                2
        );
        break;
    case XK_L:
        set_pan_x(
            app,
            (rendered_image_width(app->img->zoom.level) - app->window_width) / 2
        );
        break;
    case XK_a:
        imlib_context_set_anti_alias(imlib_context_get_anti_alias() ? 0 : 1);
        app->dirty = true;
        break;
    case XK_n:
        switch_image(app, +1);
        break;
    case XK_p:
        switch_image(app, -1);
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
            center_image(app);
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
    for (size_t i = 0; i < app->images.count; ++i) {
        imlib_context_set_image(app->images.items[i].im);
        imlib_free_image();
    }
    free(app->images.items);
    sleep_to_not_break_lf();
}

static Images load_images(size_t argc, char *argv[]) {
    if (argc < 2) {
        (void)fputs("error: no images provided\n", stderr);
        exit(EXIT_FAILURE);
    }

    auto images = (Image *)malloc(argc * sizeof(Image));
    size_t image_count = 0;
    for (size_t i = 1; i < argc; ++i) {
        auto image = imlib_load_image(argv[i]);
        if (!image) {
            (void)fprintf(stderr, "error: failed to open image %s\n", argv[i]);
            continue;
        }
        images[image_count++] = (Image){
            .pan = {.x = 0, .y = 0},
            .im = image,
            .zoom = {.level = 1.0f, .mode = ZoomManual},
        };
    }

    if (image_count == 0) {
        (void)fprintf(stderr, "error: failed to open all images\n");
        free(images);
        exit(EXIT_FAILURE);
    }

    return (Images){images, image_count};
}

int main(int argc, char *argv[]) {
    auto images = load_images((size_t)argc, argv);
    auto app = app_new(images);
    while (!app.quit) {
        app_run(&app);
    }
    app_deinit(&app);
}

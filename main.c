#include <Imlib2.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>

#define auto __auto_type

#define LENGTH(arr) (sizeof(arr) / sizeof((arr)[0]))

enum { DEFAULT_WIDTH = 800, DEFAULT_HEIGHT = 600 };

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
    float zoom_level;
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

    return (App){
        .display = display,
        .window = window,
        .window_width = DEFAULT_WIDTH,
        .window_height = DEFAULT_HEIGHT,
        .zoom_level = 1.0f,
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
        x, y, source_width, source_height, x, y, width, height
    );
}

static void render_all_updates(App const *app, Imlib_Updates updates) {
    updates = imlib_updates_merge_for_rendering(
        updates, app->window_width, app->window_height
    );
    for (auto update = updates; update;
         update = imlib_updates_get_next(update)) {
        render(app, update);
    }
    imlib_updates_free(updates);
}

static void handle_key_press(App *app, XKeyEvent *event) {
    KeySym key = 0;
    XLookupString(event, NULL, 0, &key, NULL);
    switch (key) {
    case XK_q:
        app->quit = true;
        break;
    case XK_minus:
        app->zoom_level = smaller_zoom(app->zoom_level);
        printf("%d\n", (int)(app->zoom_level * 100.0f));
        break;
    case XK_plus:
        app->zoom_level = larger_zoom(app->zoom_level);
        printf("%d\n", (int)(app->zoom_level * 100.0f));
        break;
    case XK_equal:
        app->zoom_level = 1.0f;
        printf("%d\n", (int)(app->zoom_level * 100.0f));
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
        }
    } while (XPending(app->display));

    render_all_updates(app, updates);
}

static void app_deinit(App const *app) {
    XDestroyWindow(app->display, app->window);
    XCloseDisplay(app->display);
    imlib_free_image();
}

int main(int argc, char *argv[]) {
    auto image_path = argc > 1 ? argv[1] : "image.jpg";

    auto app = app_new(image_path);
    while (!app.quit) {
        app_run(&app);
    }
    app_deinit(&app);
}

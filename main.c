#include <Imlib2.h>
#include <X11/Xlib.h>
#include <X11/keysym.h>
#include <assert.h>
#include <stdbool.h>

#define auto __auto_type

enum { DEFAULT_WIDTH = 800, DEFAULT_HEIGHT = 600 };

typedef struct {
    Display *display;
    Window window;
    int window_width, window_height;
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
        .quit = false,
    };
}

static void render(Imlib_Updates updates) {
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
    imlib_updates_get_coordinates(updates, &x, &y, &width, &height);
    imlib_render_image_part_on_drawable_at_size(
        x, y, width, height, x, y, width, height
    );
}

static void render_all_updates(App const *app, Imlib_Updates updates) {
    updates = imlib_updates_merge_for_rendering(
        updates, app->window_width, app->window_height
    );
    for (auto update = updates; update;
         update = imlib_updates_get_next(update)) {
        render(update);
    }
    imlib_updates_free(updates);
}

static void handle_key_press(App *app, XKeyEvent *event) {
    auto key = XLookupKeysym(event, 0);
    if (key == XK_q) {
        app->quit = true;
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

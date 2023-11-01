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
    bool quit;
} App;

static App app_new(void) {
    auto display = XOpenDisplay(NULL);
    assert(display && "failed to open display");

    auto screen = DefaultScreen(display);
    auto root = RootWindow(display, screen);

    auto window = XCreateSimpleWindow(
        display, root, 0, 0, DEFAULT_WIDTH, DEFAULT_HEIGHT, 0, None, None
    );

    XStoreName(display, window, "iv");
    XSelectInput(display, window, ExposureMask | KeyPressMask);
    XMapWindow(display, window);

    auto image = imlib_load_image("image.jpg");
    assert(image && "failed to open image");
    imlib_context_set_image(image);
    imlib_context_set_display(display);
    imlib_context_set_visual(DefaultVisual(display, screen));
    imlib_context_set_drawable(window);

    return (App){
        .display = display,
        .window = window,
        .quit = false,
    };
}

static void render(App const *app) {
    imlib_render_image_on_drawable(0, 0);
}

static void handle_key_press(App *app, XKeyEvent *event) {
    auto key = XLookupKeysym(event, 0);
    if (key == XK_q) {
        app->quit = true;
    }
}

static void app_run(App *app) {
    XEvent event;
    XNextEvent(app->display, &event);
    switch (event.type) {
    case Expose:
        render(app);
        break;
    case KeyPress:
        handle_key_press(app, &event.xkey);
        break;
    }
}

static void app_deinit(App const *app) {
    XDestroyWindow(app->display, app->window);
    XCloseDisplay(app->display);
    imlib_free_image();
}

int main(void) {
    auto app = app_new();
    while (!app.quit) {
        app_run(&app);
    }
    app_deinit(&app);
}

#include <GL/glew.h>
#include <GL/glx.h>
#include <Imlib2.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>
#include <limits.h>
#include <math.h>
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
    ZoomFitDownscale,
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
    GLuint shader_program;
    bool dirty;
    bool quit;
} App;

static char const *const vertex_source = "#version 150 core\n\
    in vec2 position;\n\
    out vec2 texcoord;\n\
    uniform vec2 pan;\n\
    uniform vec2 zoom;\n\
    void main() {\n\
        texcoord = position * vec2(-0.5, 0.5) + vec2(0.5);\n\
        gl_Position = vec4(-position * zoom + pan, 0.0, 1.0);\n\
    }";

static char const *const fragment_source = "#version 150 core\n\
    in vec2 texcoord;\n\
    out vec4 color;\n\
    uniform sampler2D tex;\n\
    void main() {\n\
        color = texture(tex, texcoord).bgra;\n\
    }";

static GLuint set_up_opengl(void) {
    glewInit();

    float vertices[] = {-1.0f, -1.0f, +1.0f, -1.0f, +1.0f, +1.0f,
                        +1.0f, +1.0f, -1.0f, +1.0f, -1.0f, -1.0f};
    GLuint vbo = 0;
    glGenBuffers(1, &vbo);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_STATIC_DRAW);

    auto vertex_shader = glCreateShader(GL_VERTEX_SHADER);
    glShaderSource(vertex_shader, 1, &vertex_source, NULL);
    glCompileShader(vertex_shader);

    auto fragment_shader = glCreateShader(GL_FRAGMENT_SHADER);
    glShaderSource(fragment_shader, 1, &fragment_source, NULL);
    glCompileShader(fragment_shader);

    auto shader_program = glCreateProgram();
    glAttachShader(shader_program, vertex_shader);
    glAttachShader(shader_program, fragment_shader);

    GLuint tex = 0;
    glGenTextures(1, &tex);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, tex);
    glUniform1i(glGetUniformLocation(shader_program, "tex"), 0);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

    glBindFragDataLocation(shader_program, 0, "color");

    glLinkProgram(shader_program);
    glUseProgram(shader_program);

    auto position = glGetAttribLocation(shader_program, "position");
    glVertexAttribPointer(
        position, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(float), 0
    );
    glEnableVertexAttribArray(position);

    glClearColor(0, 0, 0, 1);

    return shader_program;
}

static App app_new(Images images) {
    auto display = XOpenDisplay(NULL);
    if (!display) {
        (void)fputs("error: failed to open display\n", stderr);
        exit(EXIT_FAILURE);
    }

    auto screen = DefaultScreen(display);
    auto root = RootWindow(display, screen);

    enum { BITS_PER_PIXEL = 24 };
    GLint attributes[] = {
        GLX_RGBA, GLX_DEPTH_SIZE, BITS_PER_PIXEL, GLX_DOUBLEBUFFER, None};
    auto visual = glXChooseVisual(display, 0, attributes);
    auto glc = glXCreateContext(display, visual, NULL, GL_TRUE);

    auto cmap = XCreateColormap(display, root, visual->visual, AllocNone);
    auto swa = (XSetWindowAttributes){
        .colormap = cmap,
        .event_mask = ExposureMask | KeyPressMask | StructureNotifyMask,
    };

    auto window = XCreateWindow(
        display, root, 0, 0, DEFAULT_WIDTH, DEFAULT_HEIGHT, 0, visual->depth,
        InputOutput, visual->visual, CWColormap | CWEventMask, &swa
    );

    glXMakeCurrent(display, window, glc);

    imlib_context_set_image(images.items[0].im);

    auto shader_program = set_up_opengl();

    XStoreName(display, window, "iv");
    XSetWindowBackgroundPixmap(display, window, None);
    XMapWindow(display, window);

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
        .shader_program = shader_program,
        .dirty = false,
        .quit = false,
    };
}

static int rendered_image_width(float zoom_level) {
    return (int)((float)imlib_image_get_width() * zoom_level);
}

static int rendered_image_height(float zoom_level) {
    return (int)((float)imlib_image_get_height() * zoom_level);
}

static void render(App *const app) {
    glViewport(0, 0, app->window_width, app->window_height);
    glUniform2f(
        glGetUniformLocation(app->shader_program, "pan"),
        (float)app->img->pan.x / (float)app->window_width * 2,
        (float)app->img->pan.y / (float)app->window_height * 2
    );
    glUniform2f(
        glGetUniformLocation(app->shader_program, "zoom"),
        app->img->zoom.level * (float)imlib_image_get_width() /
            (float)app->window_width,
        app->img->zoom.level * (float)imlib_image_get_height() /
            (float)app->window_height
    );
    glClear(GL_COLOR_BUFFER_BIT);
    enum { VERTEX_COUNT = 6 };
    glDrawArrays(GL_TRIANGLES, 0, VERTEX_COUNT);
    glXSwapBuffers(app->display, app->window);
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
    auto img = app->img;
    x = rendered_image_width(app->img->zoom.level) <= app->window_width
            ? 0
            : clamp_pan_x(app, x);
    if (img->pan.x != x) {
        img->pan.x = x;
        app->dirty = true;
    }
}

static void set_pan_y(App *app, int y) {
    auto img = app->img;
    y = rendered_image_height(app->img->zoom.level) <= app->window_height
            ? 0
            : clamp_pan_y(app, y);
    if (img->pan.y != y) {
        img->pan.y = y;
        app->dirty = true;
    }
}

static void set_zoom_level(App *app, float level) {
    auto img = app->img;
    if (img->zoom.level != level) {
        img->zoom.level = level;
        set_pan_x(app, img->pan.x);
        set_pan_y(app, img->pan.y);
        app->dirty = true;
    }
}

static void switch_image(App *app, int offset) {
    auto index = (size_t)(app->img - app->images.items + offset);
    if (index >= app->images.count) {
        return;
    }
    app->img = &app->images.items[index];
    imlib_context_set_image(app->img->im);
    app->dirty = true;

    auto image_data = imlib_image_get_data_for_reading_only();
    auto width = imlib_image_get_width();
    auto height = imlib_image_get_height();
    glTexImage2D(
        GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE,
        image_data
    );
}

static void auto_zoom(App *app) {
    auto img = app->img;
    switch (img->zoom.mode) {
    case ZoomFitDownscale: {
        auto fit = fminf(
            (float)app->window_width / (float)imlib_image_get_width(),
            (float)app->window_height / (float)imlib_image_get_height()
        );
        set_zoom_level(app, fminf(fit, 1.0f));
    } break;
    default:;
    }
}

static void handle_key_press(App *app, XKeyEvent *event) {
    KeySym key = 0;
    XLookupString(event, NULL, 0, &key, NULL);
    switch (key) {
    case XK_q:
        app->quit = true;
        break;
    case XK_minus:
        app->img->zoom.mode = ZoomManual;
        set_zoom_level(app, smaller_zoom(app->img->zoom.level));
        break;
    case XK_plus:
        app->img->zoom.mode = ZoomManual;
        set_zoom_level(app, larger_zoom(app->img->zoom.level));
        break;
    case XK_equal:
        app->img->zoom.mode = ZoomManual;
        set_zoom_level(app, 1.0f);
        break;
    case XK_w:
        app->img->zoom.mode = ZoomFitDownscale;
        auto_zoom(app);
        break;
    case XK_h:
        set_pan_x(app, app->img->pan.x + app->window_width / PAN_AMOUNT);
        break;
    case XK_l:
        set_pan_x(app, app->img->pan.x - app->window_width / PAN_AMOUNT);
        break;
    case XK_k:
        set_pan_y(app, app->img->pan.y - app->window_height / PAN_AMOUNT);
        break;
    case XK_j:
        set_pan_y(app, app->img->pan.y + app->window_height / PAN_AMOUNT);
        break;
    case XK_H:
        set_pan_x(app, INT_MAX);
        break;
    case XK_K:
        set_pan_y(app, INT_MIN);
        break;
    case XK_L:
        set_pan_x(app, INT_MIN);
        break;
    case XK_J:
        set_pan_y(app, INT_MAX);
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
    do {
        XEvent event;
        XNextEvent(app->display, &event);
        switch (event.type) {
        case Expose:
            render(app);
            break;
        case KeyPress:
            handle_key_press(app, &event.xkey);
            break;
        case ConfigureNotify: {
            auto size = event.xconfigure;
            app->window_width = size.width;
            app->window_height = size.height;
            auto_zoom(app);
            set_pan_x(app, app->img->pan.x);
            set_pan_y(app, app->img->pan.y);
        } break;
        case ClientMessage:
            if ((Atom)event.xclient.data.l[0] == app->atom_wm_delete_window) {
                app->quit = true;
            }
            break;
        }
    } while (XPending(app->display));

    if (app->dirty) {
        app->dirty = false;
        XClearArea(
            app->display, app->window, 0, 0, (unsigned)app->window_width,
            (unsigned)app->window_height, true
        );
    }
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
            .zoom = {.level = 1.0f, .mode = ZoomFitDownscale},
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
    switch_image(&app, 0);
    while (!app.quit) {
        app_run(&app);
    }
    app_deinit(&app);
}

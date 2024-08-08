const std = @import("std");
const c = @cImport({
    @cInclude("GL/glew.h");
    @cInclude("GL/glx.h");
    @cInclude("Imlib2.h");
    @cInclude("unistd.h");
});

const Allocator = std.mem.Allocator;

const default_width = 800;
const default_height = 600;
const pan_amount = 5;
const zoom_levels = [_]f32{ 0.125, 0.25, 0.75, 1.0, 1.5, 2.0, 4.0, 8.0, 12.0, 16.0 };

const vertex_source =
    \\#version 150 core
    \\in vec2 position;
    \\out vec2 texcoord;
    \\uniform vec2 pan;
    \\uniform vec2 zoom;
    \\void main() {
    \\    texcoord = position * vec2(-0.5, 0.5) + vec2(0.5);
    \\    gl_Position = vec4(-position * zoom + pan, 0.0, 1.0);
    \\}
;

const fragment_source =
    \\#version 150 core
    \\in vec2 texcoord;
    \\out vec4 color;
    \\uniform sampler2D tex;
    \\void main() {
    \\    color = texture(tex, texcoord).bgra;
    \\}
;

const Image = struct {
    pan: struct {
        x: c_int,
        y: c_int,
    },
    im: c.Imlib_Image,
    zoom: struct {
        level: f32,
        mode: enum { manual, fit_downscale },
    },
};

const Images = std.ArrayList(Image);

const App = struct {
    display: *c.Display,
    window: c.Window,
    window_width: c_int,
    window_height: c_int,
    atom_wm_delete_window: c.Atom,
    img: *Image,
    images: Images,
    shader_program: c.GLuint,
    dirty: bool,
    quit: bool,

    fn new(images: Images) !@This() {
        const display = c.XOpenDisplay(null) orelse return error.FailedToOpenDisplay;
        const screen = c.DefaultScreen(display);
        const root = c.RootWindow(display, screen);
        const bits_per_pixel = 24;
        var attributes = [_]c_int{
            c.GLX_RGBA,
            c.GLX_DEPTH_SIZE,
            bits_per_pixel,
            c.GLX_DOUBLEBUFFER,
            0,
        };
        const visual = c.glXChooseVisual(display, 0, &attributes);
        const glc = c.glXCreateContext(display, visual, null, 1);
        const cmap = c.XCreateColormap(display, root, visual.*.visual, 0);
        var swa = std.mem.zeroInit(c.XSetWindowAttributes, .{
            .event_mask = c.ExposureMask | c.KeyPressMask | c.StructureNotifyMask,
            .colormap = cmap,
        });
        const window = c.XCreateWindow(
            display,
            root,
            0,
            0,
            default_width,
            default_height,
            0,
            visual.*.depth,
            1,
            visual.*.visual,
            c.CWColormap | c.CWEventMask,
            &swa,
        );
        _ = c.glXMakeCurrent(display, window, glc);
        const shader_program = setUpOpengl();
        _ = c.XStoreName(display, window, "iv");
        _ = c.XSetWindowBackgroundPixmap(display, window, 0);
        _ = c.XMapWindow(display, window);
        var atom_wm_delete_window = c.XInternAtom(display, "WM_DELETE_WINDOW", 0);
        _ = c.XSetWMProtocols(display, window, &atom_wm_delete_window, 1);
        return .{
            .display = display,
            .window = window,
            .window_width = default_width,
            .window_height = default_height,
            .atom_wm_delete_window = atom_wm_delete_window,
            .img = &images.items[0],
            .images = images,
            .shader_program = shader_program,
            .dirty = false,
            .quit = false,
        };
    }

    fn run(self: *@This()) void {
        while (true) {
            var event: c.XEvent = undefined;
            _ = c.XNextEvent(self.display, &event);
            switch (event.type) {
                c.Expose => render(self),
                c.KeyPress => handleKeyPress(self, &event.xkey),
                c.ConfigureNotify => {
                    const size = event.xconfigure;
                    self.window_width = size.width;
                    self.window_height = size.height;
                    autoZoom(self);
                    setPanX(self, self.img.pan.x);
                    setPanY(self, self.img.pan.y);
                },
                c.ClientMessage => {
                    if (event.xclient.data.l[0] == self.atom_wm_delete_window)
                        self.quit = true;
                },
                else => {},
            }
            if (c.XPending(self.display) == 0) break;
        }
        if (self.dirty) {
            self.dirty = false;
            _ = c.XClearArea(
                self.display,
                self.window,
                0,
                0,
                @intCast(self.window_width),
                @intCast(self.window_height),
                1,
            );
        }
    }

    fn deinit(self: *@This()) !void {
        _ = c.XDestroyWindow(self.display, self.window);
        _ = c.XCloseDisplay(self.display);
        for (self.images.items) |image| {
            c.imlib_context_set_image(image.im);
            c.imlib_free_image();
        }
        self.images.deinit();
        try sleepToNotBreakLf();
    }
};

fn smallerZoom(level: f32) f32 {
    var i = zoom_levels.len;
    return while (i != 0) {
        i -= 1;
        if (zoom_levels[i] < level) break zoom_levels[i];
    } else level;
}

fn largerZoom(level: f32) f32 {
    return for (zoom_levels) |l| {
        if (l > level) break l;
    } else level;
}

fn setUpOpengl() c.GLuint {
    _ = c.glewInit();

    var vertices = [_]f32{
        -1.0, -1.0,
        1.0,  -1.0,
        1.0,  1.0,
        1.0,  1.0,
        -1.0, 1.0,
        -1.0, -1.0,
    };

    var vbo: c.GLuint = 0;
    c.__glewGenBuffers.?(1, &vbo);
    c.__glewBindBuffer.?(c.GL_ARRAY_BUFFER, vbo);
    c.__glewBufferData.?(c.GL_ARRAY_BUFFER, @sizeOf(@TypeOf(vertices)), &vertices, c.GL_STATIC_DRAW);

    const vertex_shader = c.__glewCreateShader.?(c.GL_VERTEX_SHADER);
    c.__glewShaderSource.?(vertex_shader, 1, @ptrCast(&vertex_source), null);
    c.__glewCompileShader.?(vertex_shader);

    const fragment_shader = c.__glewCreateShader.?(c.GL_FRAGMENT_SHADER);
    c.__glewShaderSource.?(fragment_shader, 1, @ptrCast(&fragment_source), null);
    c.__glewCompileShader.?(fragment_shader);

    const shader_program = c.__glewCreateProgram.?();
    c.__glewAttachShader.?(shader_program, vertex_shader);
    c.__glewAttachShader.?(shader_program, fragment_shader);

    var tex: c.GLuint = 0;
    c.glGenTextures(1, &tex);
    c.__glewActiveTexture.?(c.GL_TEXTURE0);
    c.glBindTexture(c.GL_TEXTURE_2D, tex);
    c.__glewUniform1i.?(c.__glewGetUniformLocation.?(shader_program, "tex"), 0);
    c.glTexParameteri(c.GL_TEXTURE_2D, c.GL_TEXTURE_MIN_FILTER, c.GL_LINEAR);
    c.glTexParameteri(c.GL_TEXTURE_2D, c.GL_TEXTURE_MAG_FILTER, c.GL_LINEAR);

    c.__glewBindFragDataLocation.?(shader_program, 0, "color");

    c.__glewLinkProgram.?(shader_program);
    c.__glewUseProgram.?(shader_program);

    const position = c.__glewGetAttribLocation.?(shader_program, "position");
    c.__glewVertexAttribPointer.?(@bitCast(position), 2, c.GL_FLOAT, c.GL_FALSE, @sizeOf([2]f32), null);
    c.__glewEnableVertexAttribArray.?(@bitCast(position));

    c.glClearColor(0.0, 0.0, 0.0, 1.0);

    return shader_program;
}

fn renderedImageWidth(zoom_level: f32) c_int {
    return @intFromFloat(@as(f32, @floatFromInt(c.imlib_image_get_width())) * zoom_level);
}

fn renderedImageHeight(zoom_level: f32) c_int {
    return @intFromFloat(@as(f32, @floatFromInt(c.imlib_image_get_height())) * zoom_level);
}

fn render(app: *App) void {
    c.glViewport(0, 0, app.window_width, app.window_height);
    c.__glewUniform2f.?(
        c.__glewGetUniformLocation.?(app.shader_program, "pan"),
        (@as(f32, @floatFromInt(app.img.pan.x)) / @as(f32, @floatFromInt(app.window_width))) * 2.0,
        (@as(f32, @floatFromInt(app.img.pan.y)) / @as(f32, @floatFromInt(app.window_height))) * 2.0,
    );
    c.__glewUniform2f.?(
        c.__glewGetUniformLocation.?(app.shader_program, "zoom"),
        (app.img.zoom.level * @as(f32, @floatFromInt(c.imlib_image_get_width()))) / @as(f32, @floatFromInt(app.window_width)),
        (app.img.zoom.level * @as(f32, @floatFromInt(c.imlib_image_get_height()))) / @as(
            f32,
            @floatFromInt(app.window_height),
        ),
    );
    c.glClear(c.GL_COLOR_BUFFER_BIT);
    const vertex_count = 6;
    c.glDrawArrays(c.GL_TRIANGLES, 0, vertex_count);
    c.glXSwapBuffers(app.display, app.window);
}

fn clampPanX(app: *const App, x: c_int) c_int {
    const limit = @divTrunc(app.window_width - renderedImageWidth(app.img.zoom.level), 2);
    return @min(-limit, @max(limit, x));
}

fn clampPanY(app: *const App, y: c_int) c_int {
    const limit = @divTrunc(app.window_height - renderedImageHeight(app.img.zoom.level), 2);
    return @min(-limit, @max(limit, y));
}

fn setPanX(app: *App, x: c_int) void {
    var img = app.img;
    const x_ = if (renderedImageWidth(app.img.zoom.level) <= app.window_width) 0 else clampPanX(app, x);
    if (img.pan.x != x_) {
        img.pan.x = x_;
        app.dirty = true;
    }
}

fn setPanY(app: *App, y: c_int) void {
    var img = app.img;
    const y_ = if (renderedImageHeight(app.img.zoom.level) <= app.window_height) 0 else clampPanY(app, y);
    if (img.pan.y != y_) {
        img.pan.y = y_;
        app.dirty = true;
    }
}

fn setZoomLevel(app: *App, level: f32) void {
    var img = app.img;
    if (img.zoom.level != level) {
        img.zoom.level = level;
        setPanX(app, img.pan.x);
        setPanY(app, img.pan.y);
        app.dirty = true;
    }
}

fn autoZoom(app: *App) void {
    switch (app.img.zoom.mode) {
        .manual => {},
        .fit_downscale => {
            const fit: f32 = @min(
                @as(f32, @floatFromInt(app.window_width)) / @as(f32, @floatFromInt(c.imlib_image_get_width())),
                @as(f32, @floatFromInt(app.window_height)) / @as(f32, @floatFromInt(c.imlib_image_get_height())),
            );
            setZoomLevel(app, @min(fit, 1.0));
        },
    }
}

fn switchImage(app: *App, offset: isize) void {
    const index = @as(isize, @intCast(app.img - &app.images.items[0])) + offset;
    if (index < 0 or app.images.items.len <= index) return;
    app.img = &app.images.items[@intCast(index)];
    c.imlib_context_set_image(app.img.im);
    autoZoom(app);
    app.dirty = true;
    const image_data = c.imlib_image_get_data_for_reading_only();
    const width = c.imlib_image_get_width();
    const height = c.imlib_image_get_height();
    c.glTexImage2D(c.GL_TEXTURE_2D, 0, c.GL_RGBA, width, height, 0, c.GL_RGBA, c.GL_UNSIGNED_BYTE, image_data);
}

fn handleKeyPress(app: *App, event: *c.XKeyEvent) void {
    var key: c.KeySym = 0;
    _ = c.XLookupString(event, null, 0, &key, null);
    switch (key) {
        c.XK_q => app.quit = true,
        c.XK_minus => {
            app.img.zoom.mode = .manual;
            setZoomLevel(app, smallerZoom(app.img.zoom.level));
        },
        c.XK_plus => {
            app.img.zoom.mode = .manual;
            setZoomLevel(app, largerZoom(app.img.zoom.level));
        },
        c.XK_equal => {
            app.img.zoom.mode = .manual;
            setZoomLevel(app, 1.0);
        },
        c.XK_w => {
            app.img.zoom.mode = .fit_downscale;
            autoZoom(app);
        },
        c.XK_h => setPanX(app, app.img.pan.x + @divTrunc(app.window_width, pan_amount)),
        c.XK_l => setPanX(app, app.img.pan.x - @divTrunc(app.window_width, pan_amount)),
        c.XK_k => setPanY(app, app.img.pan.y - @divTrunc(app.window_height, pan_amount)),
        c.XK_j => setPanY(app, app.img.pan.y + @divTrunc(app.window_height, pan_amount)),
        c.XK_H => setPanX(app, std.math.maxInt(c_int)),
        c.XK_K => setPanY(app, std.math.minInt(c_int)),
        c.XK_L => setPanX(app, std.math.minInt(c_int)),
        c.XK_J => setPanY(app, std.math.maxInt(c_int)),
        c.XK_n => switchImage(app, 1),
        c.XK_p => switchImage(app, -1),
        else => {},
    }
}

fn sleepToNotBreakLf() !void {
    const parent_pid = c.getppid();
    const format = "/proc/{}/cmdline";
    const longest_cmdline = std.fmt.comptimePrint(format, .{std.math.maxInt(@TypeOf(parent_pid))});
    var buf: [longest_cmdline.len]u8 = undefined;
    const cmdline_path = std.fmt.bufPrint(&buf, format, .{parent_pid}) catch unreachable;
    var cmdline_file = try std.fs.openFileAbsolute(cmdline_path, .{});
    defer cmdline_file.close();
    var cmdline = std.mem.zeroes(["lf".len]u8);
    _ = try cmdline_file.read(&cmdline);
    if (std.mem.eql(u8, &cmdline, "lf")) {
        std.time.sleep(100_000_000);
    }
}

fn loadImages(allocator: Allocator) !Images {
    const args = try std.process.argsAlloc(allocator);
    defer std.process.argsFree(allocator, args);

    if (args.len < 2) return error.NoImagesProvided;

    var images = try Images.initCapacity(allocator, args.len - 1);
    errdefer images.deinit();
    for (args[1..]) |path| {
        const image = c.imlib_load_image(path) orelse {
            std.log.err("failed to open image {s}", .{path});
            continue;
        };
        images.appendAssumeCapacity(.{
            .pan = .{ .x = 0, .y = 0 },
            .im = image,
            .zoom = .{ .level = 1.0, .mode = .fit_downscale },
        });
    }

    if (images.items.len == 0) return error.FailedToOpenAllImages;
    return images;
}

pub fn main() !void {
    var gpa = std.heap.GeneralPurposeAllocator(.{}){};
    defer std.debug.assert(gpa.deinit() == .ok);
    const allocator = gpa.allocator();

    const images = try loadImages(allocator);
    var app = try App.new(images);
    switchImage(&app, 0);
    while (!app.quit) app.run();
    try app.deinit();
}

/*
 * display_probe - inspect the Brick Pro display stack.
 *
 * Reports, in order:
 *   - /dev/fb0 mode and framebuffer geometry (FBIOGET_VSCREENINFO/FSCREENINFO)
 *   - SDL video driver, requested vs actual window size
 *   - EGL vendor/version/extensions, surface size, buffer age, damage support
 *   - GLES renderer/version/extensions
 *   - one 1024x768-style clear + swap loop with swap latency statistics
 *
 * Also implements `--dump-fb FILE`, which reads the visible framebuffer and
 * writes a binary PPM. screenshot.sh uses it because the stock image has no
 * framebuffer grab tool and Android's screencap does not exist here.
 *
 * Build: see scripts/build-target.sh. C11, GLES2, no C++.
 */
#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include <linux/fb.h>

#include <SDL.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>

#ifndef EGL_BUFFER_AGE_EXT
#define EGL_BUFFER_AGE_EXT 0x313D
#endif

#define PROBE_W 1024
#define PROBE_H 768
#define DEFAULT_FRAMES 60
#define MAX_FRAMES 1000

struct fb_info {
    int fd;
    struct fb_var_screeninfo var;
    struct fb_fix_screeninfo fix;
};

static int failures;

static void fail(const char *message)
{
    printf("FAIL: %s\n", message);
    failures++;
}

static const char *or_unknown(const char *value)
{
    return value ? value : "(null)";
}

static int read_fb_info(struct fb_info *info, const char *path)
{
    memset(info, 0, sizeof(*info));
    info->fd = -1;
    info->fd = open(path, O_RDONLY);
    if (info->fd < 0) {
        printf("fb0_error: cannot open %s: %s\n", path, strerror(errno));
        return -1;
    }
    if (ioctl(info->fd, FBIOGET_VSCREENINFO, &info->var) < 0) {
        printf("fb0_error: FBIOGET_VSCREENINFO on %s: %s\n", path, strerror(errno));
        close(info->fd);
        info->fd = -1;
        return -1;
    }
    if (ioctl(info->fd, FBIOGET_FSCREENINFO, &info->fix) < 0) {
        printf("fb0_error: FBIOGET_FSCREENINFO on %s: %s\n", path, strerror(errno));
        close(info->fd);
        info->fd = -1;
        return -1;
    }
    return 0;
}

static void print_fb_info(const struct fb_info *info, const char *path)
{
    const struct fb_var_screeninfo *var = &info->var;
    const struct fb_fix_screeninfo *fix = &info->fix;

    printf("== fb0 ==\n");
    printf("fb0_path: %s\n", path);
    printf("fb0_id: %s\n", fix->id);
    printf("fb0_res: %ux%u\n", var->xres, var->yres);
    printf("fb0_virtual_res: %ux%u\n", var->xres_virtual, var->yres_virtual);
    printf("fb0_offset: %u,%u\n", var->xoffset, var->yoffset);
    printf("fb0_bpp: %u\n", var->bits_per_pixel);
    printf("fb0_line_length: %u\n", fix->line_length);
    printf("fb0_smem_len: %u\n", fix->smem_len);
    printf("fb0_channels: r=%u@%u g=%u@%u b=%u@%u a=%u@%u\n",
           var->red.length, var->red.offset,
           var->green.length, var->green.offset,
           var->blue.length, var->blue.offset,
           var->transp.length, var->transp.offset);
    printf("fb0_rotate: %u\n", var->rotate);
}

static unsigned int extract_channel(unsigned int pixel, const struct fb_bitfield *field)
{
    unsigned int mask;

    if (field->length == 0 || field->length >= 32) {
        return 0;
    }
    mask = (1u << field->length) - 1u;
    return (((pixel >> field->offset) & mask) * 255u) / mask;
}

static int dump_fb(const char *out_path)
{
    struct fb_info info;
    unsigned int bpp;
    unsigned int width;
    unsigned int height;
    unsigned int line_length;
    size_t needed;
    unsigned char *mapping;
    FILE *out;
    unsigned int y;
    int rc = 1;

    if (read_fb_info(&info, "/dev/fb0") < 0) {
        return 1;
    }
    print_fb_info(&info, "/dev/fb0");

    bpp = info.var.bits_per_pixel;
    width = info.var.xres;
    height = info.var.yres;
    line_length = info.fix.line_length;
    if ((bpp != 16 && bpp != 32) || width == 0 || height == 0 || line_length == 0) {
        fprintf(stderr, "dump-fb: unsupported framebuffer format (%ux%u %ubpp)\n",
                width, height, bpp);
        close(info.fd);
        return 1;
    }
    needed = (size_t)line_length * height;
    if (info.fix.smem_len != 0 && needed > info.fix.smem_len) {
        fprintf(stderr, "dump-fb: framebuffer too small (%zu > %u)\n",
                needed, info.fix.smem_len);
        close(info.fd);
        return 1;
    }

    mapping = mmap(NULL, needed, PROT_READ, MAP_SHARED, info.fd, 0);
    if (mapping == MAP_FAILED) {
        fprintf(stderr, "dump-fb: mmap failed: %s\n", strerror(errno));
        close(info.fd);
        return 1;
    }

    out = strcmp(out_path, "-") == 0 ? stdout : fopen(out_path, "wb");
    if (!out) {
        fprintf(stderr, "dump-fb: cannot open %s: %s\n", out_path, strerror(errno));
        munmap(mapping, needed);
        close(info.fd);
        return 1;
    }

    fprintf(out, "P6\n%u %u\n255\n", width, height);
    for (y = 0; y < height; y++) {
        const unsigned char *row = mapping + (size_t)y * line_length;
        unsigned int x;
        for (x = 0; x < width; x++) {
            unsigned int pixel;
            unsigned char rgb[3];

            if (bpp == 32) {
                uint32_t value;
                memcpy(&value, row + (size_t)x * 4, sizeof(value));
                pixel = value;
            } else {
                uint16_t value;
                memcpy(&value, row + (size_t)x * 2, sizeof(value));
                pixel = value;
            }
            rgb[0] = (unsigned char)extract_channel(pixel, &info.var.red);
            rgb[1] = (unsigned char)extract_channel(pixel, &info.var.green);
            rgb[2] = (unsigned char)extract_channel(pixel, &info.var.blue);
            if (fwrite(rgb, 1, sizeof(rgb), out) != sizeof(rgb)) {
                fprintf(stderr, "dump-fb: write failed: %s\n", strerror(errno));
                goto out_cleanup;
            }
        }
    }

    if (out != stdout && fflush(out) != 0) {
        fprintf(stderr, "dump-fb: flush failed: %s\n", strerror(errno));
        goto out_cleanup;
    }
    fprintf(stderr, "dump-fb: wrote %ux%u PPM to %s\n", width, height, out_path);
    rc = 0;

out_cleanup:
    if (out != stdout) {
        fclose(out);
    }
    munmap(mapping, needed);
    close(info.fd);
    return rc;
}

static double now_ms(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1000000.0;
}

static int compare_double(const void *a, const void *b)
{
    double left = *(const double *)a;
    double right = *(const double *)b;
    return (left > right) - (left < right);
}

static double percentile(const double *values, int count, double fraction)
{
    int index = (int)(fraction * (double)(count - 1));
    return values[index];
}

static void print_extension_presence(const char *extensions, const char *name)
{
    printf("%s: %s\n", name, strstr(extensions, name) ? "yes" : "no");
}

static void set_gl_attributes(void)
{
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_RED_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_GREEN_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_BLUE_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_ALPHA_SIZE, 0);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 0);
    SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 0);
}

/*
 * Lightweight readiness probe for the boot hook.  Creating the window and
 * context is the part that fails while the PowerVR RGX firmware is still
 * loading; a full 60-frame benchmark would add boot latency and make swap
 * timing failures look like display readiness failures.
 */
static int run_ready(void)
{
    SDL_Window *window = NULL;
    SDL_GLContext context = NULL;
    EGLSurface surface = EGL_NO_SURFACE;
    int window_w = 0;
    int window_h = 0;
    int rc = 1;

    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        printf("ready_error: SDL_Init: %s\n", SDL_GetError());
        return 1;
    }

    set_gl_attributes();
    window = SDL_CreateWindow("brickdesk display readiness",
                              SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED,
                              PROBE_W, PROBE_H, SDL_WINDOW_OPENGL);
    if (!window) {
        printf("ready_error: SDL_CreateWindow: %s\n", SDL_GetError());
        goto cleanup;
    }

    SDL_GetWindowSize(window, &window_w, &window_h);
    if (window_w != PROBE_W || window_h != PROBE_H) {
        printf("ready_error: window is %dx%d, expected %dx%d\n",
               window_w, window_h, PROBE_W, PROBE_H);
        goto cleanup;
    }

    context = SDL_GL_CreateContext(window);
    if (!context) {
        printf("ready_error: SDL_GL_CreateContext: %s\n", SDL_GetError());
        goto cleanup;
    }

    surface = eglGetCurrentSurface(EGL_DRAW);
    if (surface == EGL_NO_SURFACE) {
        printf("ready_error: EGL draw surface is not current\n");
        goto cleanup;
    }

    printf("READY: video_driver=%s window=%dx%d\n",
           or_unknown(SDL_GetCurrentVideoDriver()), window_w, window_h);
    rc = 0;

cleanup:
    if (context) {
        SDL_GL_DeleteContext(context);
    }
    if (window) {
        SDL_DestroyWindow(window);
    }
    SDL_Quit();
    return rc;
}

static int run_probe(int frames)
{
    struct fb_info fb;
    SDL_Window *window = NULL;
    SDL_GLContext context = NULL;
    EGLDisplay display = EGL_NO_DISPLAY;
    EGLSurface surface = EGL_NO_SURFACE;
    const char *egl_extensions = "";
    int window_w = 0;
    int window_h = 0;
    int surface_w = 0;
    int surface_h = 0;
    int swap_interval = -1;
    int i;
    double *timings;
    GLenum gl_error;
    unsigned char center[4] = {0, 0, 0, 0};

    if (read_fb_info(&fb, "/dev/fb0") < 0) {
        fail("cannot read /dev/fb0");
    } else {
        print_fb_info(&fb, "/dev/fb0");
    }

    printf("== SDL ==\n");
    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        printf("sdl_error: SDL_Init: %s\n", SDL_GetError());
        fail("SDL_Init failed");
        if (fb.fd >= 0) {
            close(fb.fd);
        }
        return 1;
    }

    printf("sdl_version: %d.%d.%d\n", SDL_MAJOR_VERSION, SDL_MINOR_VERSION, SDL_PATCHLEVEL);
    printf("sdl_video_driver: %s\n", or_unknown(SDL_GetCurrentVideoDriver()));
    printf("sdl_video_drivers:");
    for (i = 0; i < SDL_GetNumVideoDrivers(); i++) {
        printf(" %s", SDL_GetVideoDriver(i));
    }
    printf("\n");

    set_gl_attributes();

    printf("sdl_requested_window: %dx%d\n", PROBE_W, PROBE_H);
    window = SDL_CreateWindow("brickdesk display probe",
                              SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED,
                              PROBE_W, PROBE_H, SDL_WINDOW_OPENGL);
    if (!window) {
        printf("sdl_error: SDL_CreateWindow: %s\n", SDL_GetError());
        fail("SDL_CreateWindow failed");
        goto cleanup;
    }
    SDL_GetWindowSize(window, &window_w, &window_h);
    printf("sdl_window_size: %dx%d\n", window_w, window_h);

    context = SDL_GL_CreateContext(window);
    if (!context) {
        printf("sdl_error: SDL_GL_CreateContext: %s\n", SDL_GetError());
        fail("SDL_GL_CreateContext failed");
        goto cleanup;
    }

    display = eglGetCurrentDisplay();
    surface = eglGetCurrentSurface(EGL_DRAW);
    if (display == EGL_NO_DISPLAY) {
        fail("eglGetCurrentDisplay returned EGL_NO_DISPLAY");
    }
    if (surface == EGL_NO_SURFACE) {
        fail("eglGetCurrentSurface returned EGL_NO_SURFACE");
    }

    printf("== EGL ==\n");
    if (display != EGL_NO_DISPLAY) {
        egl_extensions = eglQueryString(display, EGL_EXTENSIONS);
        if (!egl_extensions) {
            egl_extensions = "";
        }
        printf("egl_vendor: %s\n", or_unknown(eglQueryString(display, EGL_VENDOR)));
        printf("egl_version: %s\n", or_unknown(eglQueryString(display, EGL_VERSION)));
        printf("egl_client_apis: %s\n", or_unknown(eglQueryString(display, EGL_CLIENT_APIS)));
        printf("egl_extensions: %s\n", egl_extensions);
        print_extension_presence(egl_extensions, "EGL_EXT_buffer_age");
        print_extension_presence(egl_extensions, "EGL_KHR_swap_buffers_with_damage");
        print_extension_presence(egl_extensions, "EGL_KHR_partial_update");
        printf("egl_proc_swap_with_damage: %s\n",
               eglGetProcAddress("eglSwapBuffersWithDamageKHR") ? "yes" : "no");
        printf("egl_proc_set_damage_region: %s\n",
               eglGetProcAddress("eglSetDamageRegionKHR") ? "yes" : "no");
    }

    if (surface != EGL_NO_SURFACE && display != EGL_NO_DISPLAY) {
        if (eglQuerySurface(display, surface, EGL_WIDTH, &surface_w) &&
            eglQuerySurface(display, surface, EGL_HEIGHT, &surface_h)) {
            printf("egl_surface_size: %dx%d\n", surface_w, surface_h);
        } else {
            printf("egl_surface_error: eglQuerySurface failed: 0x%x\n", eglGetError());
            fail("eglQuerySurface(EGL_WIDTH/EGL_HEIGHT) failed");
        }
        if (strstr(egl_extensions, "EGL_EXT_buffer_age")) {
            EGLint age = -1;
            if (eglQuerySurface(display, surface, EGL_BUFFER_AGE_EXT, &age)) {
                printf("egl_buffer_age: %d\n", (int)age);
            } else {
                printf("egl_buffer_age_error: 0x%x\n", eglGetError());
            }
        }
    }

    printf("== GL ==\n");
    printf("gl_vendor: %s\n", or_unknown((const char *)glGetString(GL_VENDOR)));
    printf("gl_renderer: %s\n", or_unknown((const char *)glGetString(GL_RENDERER)));
    printf("gl_version: %s\n", or_unknown((const char *)glGetString(GL_VERSION)));
    printf("gl_shading_language: %s\n",
           or_unknown((const char *)glGetString(GL_SHADING_LANGUAGE_VERSION)));
    {
        const GLubyte *gl_extensions = glGetString(GL_EXTENSIONS);
        printf("gl_extensions: %s\n",
               gl_extensions ? (const char *)gl_extensions
                             : "(unavailable for this context)");
    }
    gl_error = glGetError();
    printf("gl_error_after_query: 0x%x\n", (unsigned int)gl_error);

    if (window_w != PROBE_W || window_h != PROBE_H) {
        printf("window_size_note: expected %dx%d, got %dx%d\n",
               PROBE_W, PROBE_H, window_w, window_h);
        fail("SDL window size does not match the 1024x768 panel");
    }
    if (fb.fd >= 0) {
        if (window_w != (int)fb.var.xres || window_h != (int)fb.var.yres) {
            fail("SDL window size does not match fb0 visible mode");
        }
        if (surface_w != (int)fb.var.xres || surface_h != (int)fb.var.yres) {
            fail("EGL surface size does not match fb0 visible mode");
        }
    }

    if (SDL_GL_SetSwapInterval(1) != 0) {
        printf("sdl_swap_interval_error: %s\n", SDL_GetError());
        SDL_ClearError();
    }
    swap_interval = SDL_GL_GetSwapInterval();
    printf("sdl_swap_interval: %d\n", swap_interval);

    printf("== clear + swap ==\n");
    glViewport(0, 0, window_w, window_h);
    glClearColor(0.10f, 0.40f, 0.80f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    glFinish();
    glReadPixels(window_w / 2, window_h / 2, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, center);
    printf("gl_center_pixel: %u,%u,%u,%u\n",
           (unsigned int)center[0], (unsigned int)center[1],
           (unsigned int)center[2], (unsigned int)center[3]);
    printf("swap_size: %dx%d\n", window_w, window_h);
    printf("swap_frames: %d (+3 warmup)\n", frames);

    timings = calloc((size_t)frames, sizeof(*timings));
    if (!timings) {
        fail("out of memory");
        goto cleanup;
    }
    for (i = 0; i < 3; i++) {
        SDL_GL_SwapWindow(window);
    }
    for (i = 0; i < frames; i++) {
        double start;
        double end;

        glClear(GL_COLOR_BUFFER_BIT);
        glFinish();
        start = now_ms();
        SDL_GL_SwapWindow(window);
        end = now_ms();
        timings[i] = end - start;
    }

    {
        double sum = 0.0;
        double min = timings[0];
        double max = timings[0];

        for (i = 0; i < frames; i++) {
            sum += timings[i];
            if (timings[i] < min) {
                min = timings[i];
            }
            if (timings[i] > max) {
                max = timings[i];
            }
        }
        qsort(timings, (size_t)frames, sizeof(*timings), compare_double);
        printf("swap_ms_min: %.3f\n", min);
        printf("swap_ms_avg: %.3f\n", sum / (double)frames);
        printf("swap_ms_p50: %.3f\n", percentile(timings, frames, 0.50));
        printf("swap_ms_p95: %.3f\n", percentile(timings, frames, 0.95));
        printf("swap_ms_max: %.3f\n", max);
        if (sum / (double)frames >= 100.0) {
            fail("average swap time is >= 100 ms");
        }
    }
    free(timings);

cleanup:
    if (context) {
        SDL_GL_DeleteContext(context);
    }
    if (window) {
        SDL_DestroyWindow(window);
    }
    SDL_Quit();
    if (fb.fd >= 0) {
        close(fb.fd);
    }

    if (failures == 0) {
        printf("RESULT: PASS\n");
        return 0;
    }
    printf("RESULT: FAIL (%d problem(s))\n", failures);
    return 1;
}

static void usage(const char *argv0)
{
    printf("usage: %s [--frames N]\n", argv0);
    printf("       %s --ready\n", argv0);
    printf("       %s --dump-fb FILE\n", argv0);
    printf("\n");
    printf("  --frames N     swap samples for the latency statistics (default %d)\n", DEFAULT_FRAMES);
    printf("  --ready        only create the SDL window/GL context/EGL surface; no swap\n");
    printf("  --dump-fb FILE write the visible /dev/fb0 contents as binary PPM ('-' = stdout)\n");
}

int main(int argc, char **argv)
{
    int frames = DEFAULT_FRAMES;
    int i;

    if (argc == 2 && strcmp(argv[1], "--ready") == 0) {
        return run_ready();
    }
    if (argc > 1 && strcmp(argv[1], "--ready") == 0) {
        fprintf(stderr, "error: --ready cannot be combined with other options\n");
        return 2;
    }

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--dump-fb") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "error: --dump-fb needs a file name\n");
                return 2;
            }
            return dump_fb(argv[i + 1]);
        }
        if (strcmp(argv[i], "--frames") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "error: --frames needs a number\n");
                return 2;
            }
            frames = atoi(argv[++i]);
            if (frames <= 0 || frames > MAX_FRAMES) {
                fprintf(stderr, "error: --frames must be between 1 and %d\n", MAX_FRAMES);
                return 2;
            }
            continue;
        }
        if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            usage(argv[0]);
            return 0;
        }
        fprintf(stderr, "error: unknown argument: %s\n", argv[i]);
        usage(argv[0]);
        return 2;
    }

    return run_probe(frames);
}

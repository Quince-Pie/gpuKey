#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>
#include <fcntl.h>
#include <gbm.h>
#include <libavutil/hwcontext.h>
#include <link.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define CHECK(x)                                                                                   \
    do {                                                                                           \
        if (!(x)) {                                                                                \
            fprintf(stderr, "%s:%d: %s; EGL 0x%x GL 0x%x\n", __FILE__, __LINE__, #x,               \
                    (unsigned)eglGetError(), (unsigned)glGetError());                              \
            abort();                                                                               \
        }                                                                                          \
    } while (0)

static pthread_barrier_t start;
struct task {
    unsigned index;
    const char *device;
};

struct driver_check {
    const char *prefix;
    bool found, wrong;
};

static int inspect_driver(struct dl_phdr_info *info, size_t size, void *opaque)
{
    (void)size;
    struct driver_check *check = opaque;
    const char *name = info->dlpi_name;
    if (strstr(name, "libgallium-") || strstr(name, "radeonsi_drv_video.so")) {
        size_t length = strlen(check->prefix);
        check->found = true;
        if (strncmp(name, check->prefix, length) || name[length] != '/')
            check->wrong = true;
    }
    return 0;
}

static void verify_driver(void)
{
    const char *prefix = getenv("GPUKEY_EXPECT_DRIVER");
    if (!prefix || !*prefix)
        return;
    struct driver_check check = {.prefix = prefix};
    dl_iterate_phdr(inspect_driver, &check);
    if (!check.found || check.wrong) {
        fprintf(stderr, "graphics probe loaded an unexpected Gallium driver; expected %s\n",
                prefix);
        abort();
    }
}

static GLuint shader(GLenum kind, const char *source)
{
    GLuint object = glCreateShader(kind);
    CHECK(object);
    glShaderSource(object, 1, &source, nullptr);
    glCompileShader(object);
    GLint ok = 0;
    glGetShaderiv(object, GL_COMPILE_STATUS, &ok);
    CHECK(ok);
    return object;
}

static void *draw(void *opaque)
{
    const struct task *task = opaque;
    int fd = open(task->device, O_RDWR | O_CLOEXEC);
    CHECK(fd >= 0);
    struct gbm_device *gbm = gbm_create_device(fd);
    CHECK(gbm);
    EGLDisplay display = eglGetPlatformDisplay(EGL_PLATFORM_GBM_KHR, gbm, nullptr);
    CHECK(display != EGL_NO_DISPLAY && eglInitialize(display, nullptr, nullptr));
    verify_driver();
    CHECK(eglBindAPI(EGL_OPENGL_ES_API));
    static constexpr EGLint attributes[] = {
        EGL_SURFACE_TYPE, 0, EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT,
        EGL_RED_SIZE,     8, EGL_GREEN_SIZE,      8,
        EGL_BLUE_SIZE,    8, EGL_ALPHA_SIZE,      8,
        EGL_NONE};
    EGLConfig config;
    EGLint count;
    CHECK(eglChooseConfig(display, attributes, &config, 1, &count) && count == 1);
    int barrier = pthread_barrier_wait(&start);
    CHECK(barrier == 0 || barrier == PTHREAD_BARRIER_SERIAL_THREAD);
    static constexpr EGLint context_options[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
    static constexpr char vertex[] = "#version 300 es\n"
                                     "const vec2 p[3]=vec2[3](vec2(-1,-1),vec2(3,-1),vec2(-1,3));\n"
                                     "void main(){gl_Position=vec4(p[gl_VertexID],0,1);}\n";
    static constexpr char fragment[] = "#version 300 es\nprecision highp float;\n"
                                       "uniform vec4 color; out vec4 result;\n"
                                       "void main(){result=color;}\n";
    for (unsigned round = 0; round < 4; ++round) {
        EGLContext context = eglCreateContext(display, config, EGL_NO_CONTEXT, context_options);
        if (getenv("GPUKEY_EXPECT_CONTEXT_OOM")) {
            CHECK(context == EGL_NO_CONTEXT && eglGetError() == EGL_BAD_ALLOC);
            break;
        }
        CHECK(context != EGL_NO_CONTEXT);
        CHECK(eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, context));
        const char *renderer = (const char *)glGetString(GL_RENDERER);
        CHECK(renderer && strstr(renderer, "radeonsi"));
        GLuint target, framebuffer;
        glGenRenderbuffers(1, &target);
        glBindRenderbuffer(GL_RENDERBUFFER, target);
        glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8, 16, 16);
        glGenFramebuffers(1, &framebuffer);
        glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, target);
        CHECK(glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE);
        GLuint vs = shader(GL_VERTEX_SHADER, vertex), fs = shader(GL_FRAGMENT_SHADER, fragment);
        GLuint program = glCreateProgram();
        CHECK(program);
        glAttachShader(program, vs);
        glAttachShader(program, fs);
        glLinkProgram(program);
        GLint linked = 0;
        glGetProgramiv(program, GL_LINK_STATUS, &linked);
        CHECK(linked);
        glUseProgram(program);
        GLint uniform = glGetUniformLocation(program, "color");
        CHECK(uniform >= 0);
        glViewport(0, 0, 16, 16);
        for (unsigned frame = 0; frame < 8; ++frame) {
            unsigned code = (task->index + round + frame) & 7;
            glUniform4f(uniform, (code & 1) ? 1.0f : 0.0f, (code & 2) ? 1.0f : 0.0f,
                        (code & 4) ? 1.0f : 0.0f, 1.0f);
            glDrawArrays(GL_TRIANGLES, 0, 3);
            unsigned char pixels[16 * 16 * 4];
            glReadPixels(0, 0, 16, 16, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
            CHECK(glGetError() == GL_NO_ERROR);
            for (unsigned pixel = 0; pixel < 16 * 16; ++pixel) {
                CHECK(pixels[pixel * 4] == ((code & 1) ? 255 : 0));
                CHECK(pixels[pixel * 4 + 1] == ((code & 2) ? 255 : 0));
                CHECK(pixels[pixel * 4 + 2] == ((code & 4) ? 255 : 0));
                CHECK(pixels[pixel * 4 + 3] == 255);
            }
        }
        /* Also tear down with a submitted, unread draw. Reclamation must keep
         * its resources alive until the driver's actual completion condition. */
        glDrawArrays(GL_TRIANGLES, 0, 3);
        glDeleteProgram(program);
        glDeleteShader(vs);
        glDeleteShader(fs);
        glDeleteFramebuffers(1, &framebuffer);
        glDeleteRenderbuffers(1, &target);
        CHECK(eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT));
        CHECK(eglDestroyContext(display, context));
    }
    CHECK(eglTerminate(display) && eglReleaseThread());
    gbm_device_destroy(gbm);
    CHECK(close(fd) == 0);
    return nullptr;
}

int main(int argc, char **argv)
{
    if (argc > 2)
        return 2;
    const char *device = argc == 2 ? argv[1] : "/dev/dri/renderD128";
    /* Keep a media client alive while distinct GBM/EGL displays create and
     * retire graphics contexts concurrently on the same device. */
    AVBufferRef *media = nullptr;
    CHECK(av_hwdevice_ctx_create(&media, AV_HWDEVICE_TYPE_VAAPI, device, nullptr, 0) == 0);
    constexpr unsigned threads = 8;
    CHECK(pthread_barrier_init(&start, nullptr, threads) == 0);
    pthread_t workers[threads];
    struct task tasks[threads];
    for (unsigned i = 0; i < threads; ++i) {
        tasks[i] = (struct task){i, device};
        CHECK(pthread_create(&workers[i], nullptr, draw, &tasks[i]) == 0);
    }
    for (unsigned i = 0; i < threads; ++i)
        CHECK(pthread_join(workers[i], nullptr) == 0);
    CHECK(pthread_barrier_destroy(&start) == 0);
    av_buffer_unref(&media);
    puts(getenv("GPUKEY_EXPECT_CONTEXT_OOM")
             ? "graphics: concurrent context allocation failure and cleanup pass"
             : "graphics: 32 contexts on 8 threads, 256 verified draws, in-flight teardown pass");
    return 0;
}

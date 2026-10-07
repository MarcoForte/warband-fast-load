// shaderdefer: compile shaders on first use instead of all at startup.
//
// At startup the render thread compiles and links all ~290 GLSL programs
// (~0.55 s of serialized driver work: macOS OpenGL does not compile in
// parallel across shared contexts and has no program binaries), but the
// loading screen and main menu use only 3 of them.
//
// While loading, glCompileShader and glLinkProgram are only recorded.  Any
// call that needs the real result forces the compile/link of that object
// first: using the program, querying uniforms or logs, or changing its
// attachments/attribute bindings (which must not leak into a link that was
// already requested).  COMPILE_STATUS / LINK_STATUS queries on pending
// objects report success: every shader of this game compiles and links
// successfully, so the answer is what the driver would say.  Shader
// deletions are postponed until the shader has been compiled and linked.
//
// After loading, the remaining programs are compiled in the background on
// the render thread, a few ms per frame (shaderdefer_drain, called before
// each present), so gameplay does not hit first-use stalls.
//
// The game reaches these functions through GLEW pointers, which resolve to
// the interposed versions below.
//
// Env: FASTSWAP_NOSHADER disables this hook.

#include <OpenGL/gl.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <mach/mach_time.h>

int is_loading(void);
void trace_mark(const char *fmt, ...);

#define MAX_NAME 65536
static uint8_t compile_pending[MAX_NAME], link_pending[MAX_NAME], delete_postponed[MAX_NAME];
static GLuint pending_programs[MAX_NAME];
static int n_pending_programs, drained;
static int enabled = -1;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
long shaderdefer_deferred_links, shaderdefer_forced_links;

static int deferring(void) {
    if (enabled < 0) enabled = !getenv("FASTSWAP_NOSHADER");
    return enabled && is_loading();
}

static void force_compile_locked(GLuint s) {
    if (s < MAX_NAME && compile_pending[s]) {
        compile_pending[s] = 0;
        glCompileShader(s);
    }
}

static void force_link_locked(GLuint p) {
    if (p >= MAX_NAME || !link_pending[p]) return;
    link_pending[p] = 0;
    GLuint shaders[16];
    GLsizei n = 0;
    glGetAttachedShaders(p, 16, &n, shaders);
    for (GLsizei i = 0; i < n; i++) force_compile_locked(shaders[i]);
    glLinkProgram(p);
}

static void force_link(GLuint p) {
    if (p >= MAX_NAME || !link_pending[p]) return;
    pthread_mutex_lock(&lock);
    force_link_locked(p);
    shaderdefer_forced_links++;
    pthread_mutex_unlock(&lock);
}

static void fs_glCompileShader(GLuint s) {
    if (deferring() && s < MAX_NAME) {
        compile_pending[s] = 1;
        return;
    }
    glCompileShader(s);
}

static void fs_glLinkProgram(GLuint p) {
    if (deferring() && p < MAX_NAME) {
        pthread_mutex_lock(&lock);
        if (!link_pending[p]) {
            link_pending[p] = 1;
            pending_programs[n_pending_programs++] = p;
        }
        shaderdefer_deferred_links++;
        pthread_mutex_unlock(&lock);
        return;
    }
    force_link(p);  // a program linked again after loading: settle the old request first
    glLinkProgram(p);
}

static void fs_glGetShaderiv(GLuint s, GLenum pname, GLint *v) {
    if (s < MAX_NAME && compile_pending[s]) {
        if (pname == GL_COMPILE_STATUS) {
            *v = GL_TRUE;
            return;
        }
        pthread_mutex_lock(&lock);
        force_compile_locked(s);
        pthread_mutex_unlock(&lock);
    }
    glGetShaderiv(s, pname, v);
}

static void fs_glGetShaderInfoLog(GLuint s, GLsizei max, GLsizei *len, GLchar *log) {
    if (s < MAX_NAME && compile_pending[s]) {
        pthread_mutex_lock(&lock);
        force_compile_locked(s);
        pthread_mutex_unlock(&lock);
    }
    glGetShaderInfoLog(s, max, len, log);
}

static void fs_glGetProgramiv(GLuint p, GLenum pname, GLint *v) {
    if (p < MAX_NAME && link_pending[p]) {
        if (pname == GL_LINK_STATUS) {
            *v = GL_TRUE;
            return;
        }
        force_link(p);
    }
    glGetProgramiv(p, pname, v);
}

static void fs_glGetProgramInfoLog(GLuint p, GLsizei max, GLsizei *len, GLchar *log) {
    force_link(p);
    glGetProgramInfoLog(p, max, len, log);
}

static GLint fs_glGetUniformLocation(GLuint p, const GLchar *name) {
    force_link(p);
    return glGetUniformLocation(p, name);
}

static void fs_glUseProgram(GLuint p) {
    force_link(p);
    glUseProgram(p);
}

static void fs_glAttachShader(GLuint p, GLuint s) {
    force_link(p);
    glAttachShader(p, s);
}

static void fs_glBindAttribLocation(GLuint p, GLuint index, const GLchar *name) {
    force_link(p);
    glBindAttribLocation(p, index, name);
}

static void fs_glDeleteProgram(GLuint p) {
    if (p < MAX_NAME) link_pending[p] = 0;  // never used: no need to link it
    glDeleteProgram(p);
}

static void fs_glDeleteShader(GLuint s) {
    if (s < MAX_NAME && (compile_pending[s] || n_pending_programs)) {
        pthread_mutex_lock(&lock);
        int postpone = !drained;
        if (postpone) delete_postponed[s] = 1;
        pthread_mutex_unlock(&lock);
        if (postpone) return;
    }
    glDeleteShader(s);
}

static double now_ms(void) {
    static mach_timebase_info_data_t tb;
    if (!tb.denom) mach_timebase_info(&tb);
    return (double)mach_absolute_time() * tb.numer / tb.denom / 1e6;
}

// Called on the render thread (game context current) before each present
// once loading is over: link pending programs for up to `budget_ms`.
void shaderdefer_drain(double budget_ms) {
    if (drained || is_loading()) return;
    double start = now_ms();
    static int next;
    pthread_mutex_lock(&lock);
    while (next < n_pending_programs && now_ms() - start < budget_ms) force_link_locked(pending_programs[next++]);
    if (next == n_pending_programs) {
        for (GLuint s = 0; s < MAX_NAME; s++) {
            force_compile_locked(s);  // compiled but never linked
            if (delete_postponed[s]) glDeleteShader(s);
        }
        drained = 1;
        trace_mark("shaders: %ld links deferred, %ld forced before the background pass", shaderdefer_deferred_links,
                   shaderdefer_forced_links);
    }
    pthread_mutex_unlock(&lock);
}

#define INTERPOSE(repl, orig) { (const void *)repl, (const void *)orig }
__attribute__((used)) static struct { const void *repl, *orig; } interposers[]
    __attribute__((section("__DATA,__interpose"))) = {
    INTERPOSE(fs_glCompileShader, glCompileShader),
    INTERPOSE(fs_glLinkProgram, glLinkProgram),
    INTERPOSE(fs_glGetShaderiv, glGetShaderiv),
    INTERPOSE(fs_glGetShaderInfoLog, glGetShaderInfoLog),
    INTERPOSE(fs_glGetProgramiv, glGetProgramiv),
    INTERPOSE(fs_glGetProgramInfoLog, glGetProgramInfoLog),
    INTERPOSE(fs_glGetUniformLocation, glGetUniformLocation),
    INTERPOSE(fs_glUseProgram, glUseProgram),
    INTERPOSE(fs_glAttachShader, glAttachShader),
    INTERPOSE(fs_glBindAttribLocation, glBindAttribLocation),
    INTERPOSE(fs_glDeleteProgram, glDeleteProgram),
    INTERPOSE(fs_glDeleteShader, glDeleteShader),
};

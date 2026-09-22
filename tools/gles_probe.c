// What the device's GLES/EGL stack can actually do, asked at RUNTIME.
//
//   tools/gles_probe.sh [ssh-host]      (default: rgdsplus)
//
// Written because extension strings inside a .so are not what a driver
// exposes: the A30 probe (speed-first scoping doc SS3.34) read `nm` output on
// a busybox system that has no `nm`, and every answer was a false negative.
// This links -lEGL -lGLESv2 DYNAMICALLY so glvnd and the loader pick the same
// vendor an application would get, rather than dlopening a path we chose.
//
// The question it exists to answer: can GLES EXPORT a dma-buf
// (EGL_MESA_image_dma_buf_export), not merely import one? Import is what the
// present stage needs today -- the scanout buffer is allocated from CMA/DRM
// and imported. Export is what a GLES-rendered layer would need to hand a
// buffer to someone else. They are different extensions and, on this device,
// different vendors: Mesa advertises both, the Mali blob only import.
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static int has(const char* exts, const char* want) {
  if (!exts || !want) return 0;
  const size_t n = strlen(want);
  for (const char* p = exts; (p = strstr(p, want)); p += n)
    if ((p == exts || p[-1] == ' ') && (p[n] == ' ' || p[n] == '\0')) return 1;
  return 0;
}

static void report(const char* label, const char* exts, const char* const* want, int n) {
  for (int i = 0; i < n; ++i)
    printf("  %-8s %-44s %s\n", label, want[i], has(exts, want[i]) ? "YES" : "no");
}

int main(void) {
  // Surfaceless first: no window system, no compositor connection, which is
  // what we want from a capability probe run over ssh. Fall back to the
  // default display if the platform extension is absent.
  EGLDisplay dpy = EGL_NO_DISPLAY;
  const char* client_exts = eglQueryString(EGL_NO_DISPLAY, EGL_EXTENSIONS);
  printf("EGL client extensions: %s\n\n", client_exts ? client_exts : "(none; no EGL_EXT_client_extensions)");

  if (has(client_exts, "EGL_MESA_platform_surfaceless")) {
    PFNEGLGETPLATFORMDISPLAYEXTPROC get =
        (PFNEGLGETPLATFORMDISPLAYEXTPROC)eglGetProcAddress("eglGetPlatformDisplayEXT");
    if (get) dpy = get(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, NULL);
    printf("display: surfaceless platform %s\n", dpy != EGL_NO_DISPLAY ? "OK" : "FAILED");
  }
  if (dpy == EGL_NO_DISPLAY) {
    dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    printf("display: default %s\n", dpy != EGL_NO_DISPLAY ? "OK" : "FAILED");
  }
  if (dpy == EGL_NO_DISPLAY) { printf("FATAL: no EGLDisplay\n"); return 1; }

  EGLint major = 0, minor = 0;
  if (!eglInitialize(dpy, &major, &minor)) { printf("FATAL: eglInitialize failed (0x%x)\n", eglGetError()); return 1; }
  printf("EGL %d.%d  vendor=%s  version=%s\n", major, minor,
         eglQueryString(dpy, EGL_VENDOR), eglQueryString(dpy, EGL_VERSION));
  printf("client APIs: %s\n\n", eglQueryString(dpy, EGL_CLIENT_APIS));

  const char* dpy_exts = eglQueryString(dpy, EGL_EXTENSIONS);
  static const char* const egl_want[] = {
      "EGL_EXT_image_dma_buf_import",
      "EGL_EXT_image_dma_buf_import_modifiers",
      "EGL_MESA_image_dma_buf_export",
      "EGL_KHR_image_base",
      "EGL_KHR_gl_texture_2D_image",
      "EGL_KHR_fence_sync",
      "EGL_ANDROID_native_fence_sync",
  };
  printf("EGL display extensions:\n");
  report("egl", dpy_exts, egl_want, (int)(sizeof egl_want / sizeof *egl_want));

  // A context, so GL_* can be asked. Surfaceless needs no config surface, but
  // a config is still required to create the context.
  eglBindAPI(EGL_OPENGL_ES_API);
  const EGLint cfg_attr[] = {EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
                             EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8, EGL_NONE};
  EGLConfig cfg; EGLint ncfg = 0;
  if (!eglChooseConfig(dpy, cfg_attr, &cfg, 1, &ncfg) || ncfg < 1) {
    printf("\nno EGLConfig; stopping after the EGL half (0x%x)\n", eglGetError());
    return 0;
  }
  // Ask for ES3 first, fall back to ES2: the version we get is the answer.
  const EGLint es3[] = {EGL_CONTEXT_MAJOR_VERSION, 3, EGL_NONE};
  const EGLint es2[] = {EGL_CONTEXT_MAJOR_VERSION, 2, EGL_NONE};
  EGLContext ctx = eglCreateContext(dpy, cfg, EGL_NO_CONTEXT, es3);
  if (ctx == EGL_NO_CONTEXT) ctx = eglCreateContext(dpy, cfg, EGL_NO_CONTEXT, es2);
  if (ctx == EGL_NO_CONTEXT) { printf("\nno context (0x%x)\n", eglGetError()); return 0; }
  if (!eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx)) {
    printf("\nmakeCurrent surfaceless failed (0x%x)\n", eglGetError());
    return 0;
  }

  printf("\nGL_VERSION  : %s\n", (const char*)glGetString(GL_VERSION));
  printf("GL_RENDERER : %s\n", (const char*)glGetString(GL_RENDERER));
  printf("GL_VENDOR   : %s\n", (const char*)glGetString(GL_VENDOR));
  const char* gl_exts = (const char*)glGetString(GL_EXTENSIONS);
  static const char* const gl_want[] = {
      "GL_OES_EGL_image",
      "GL_OES_EGL_image_external",
      "GL_EXT_texture_format_BGRA8888",
      "GL_OES_rgb8_rgba8",
      "GL_OES_texture_npot",
  };
  printf("\nGL extensions:\n");
  report("gl", gl_exts, gl_want, (int)(sizeof gl_want / sizeof *gl_want));

  // The actual export test, which is the point of this probe. A claim in an
  // extension string is not a working path: make a texture, wrap it as an
  // EGLImage, and ask for a dma-buf back.
  printf("\ndma-buf EXPORT, tried for real:\n");
  if (!has(dpy_exts, "EGL_MESA_image_dma_buf_export")) {
    printf("  extension absent -- not attempted\n");
  } else {
    PFNEGLCREATEIMAGEKHRPROC create = (PFNEGLCREATEIMAGEKHRPROC)eglGetProcAddress("eglCreateImageKHR");
    PFNEGLDESTROYIMAGEKHRPROC destroy = (PFNEGLDESTROYIMAGEKHRPROC)eglGetProcAddress("eglDestroyImageKHR");
    PFNEGLEXPORTDMABUFIMAGEQUERYMESAPROC q =
        (PFNEGLEXPORTDMABUFIMAGEQUERYMESAPROC)eglGetProcAddress("eglExportDMABUFImageQueryMESA");
    PFNEGLEXPORTDMABUFIMAGEMESAPROC ex =
        (PFNEGLEXPORTDMABUFIMAGEMESAPROC)eglGetProcAddress("eglExportDMABUFImageMESA");
    if (!create || !q || !ex) { printf("  entry points missing (create=%p query=%p export=%p)\n",
                                       (void*)create, (void*)q, (void*)ex); return 0; }
    GLuint tex = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 256, 192, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    const EGLint img_attr[] = {EGL_IMAGE_PRESERVED_KHR, EGL_TRUE, EGL_NONE};
    EGLImageKHR img = create(dpy, ctx, EGL_GL_TEXTURE_2D_KHR, (EGLClientBuffer)(size_t)tex, img_attr);
    if (img == EGL_NO_IMAGE_KHR) { printf("  eglCreateImageKHR failed (0x%x)\n", eglGetError()); return 0; }
    int fourcc = 0, nplanes = 0; EGLuint64KHR mods[4] = {0};
    if (!q(dpy, img, &fourcc, &nplanes, mods)) {
      printf("  eglExportDMABUFImageQueryMESA failed (0x%x)\n", eglGetError());
    } else {
      int fds[4] = {-1, -1, -1, -1}; EGLint strides[4] = {0}, offsets[4] = {0};
      if (!ex(dpy, img, fds, strides, offsets)) {
        printf("  query OK (fourcc %c%c%c%c, %d plane(s), modifier 0x%llx) but export FAILED (0x%x)\n",
               fourcc & 0xff, (fourcc >> 8) & 0xff, (fourcc >> 16) & 0xff, (fourcc >> 24) & 0xff,
               nplanes, (unsigned long long)mods[0], eglGetError());
      } else {
        printf("  EXPORT OK: fourcc %c%c%c%c, %d plane(s), modifier 0x%llx, fd %d, stride %d, offset %d\n",
               fourcc & 0xff, (fourcc >> 8) & 0xff, (fourcc >> 16) & 0xff, (fourcc >> 24) & 0xff,
               nplanes, (unsigned long long)mods[0], fds[0], strides[0], offsets[0]);
        printf("  (a 256x192 RGBA texture; stride %d means %s)\n", strides[0],
               strides[0] == 256 * 4 ? "tightly packed, as the present path wants" : "padded");
        for (int i = 0; i < nplanes; ++i) if (fds[i] >= 0) close(fds[i]);
      }
    }
    if (destroy) destroy(dpy, img);
    glDeleteTextures(1, &tex);
  }
  return 0;
}

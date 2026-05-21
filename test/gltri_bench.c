/*
 * gltri_bench - tiny GLES2 throughput benchmark for the Surface RT.
 *
 * Renders a batch of spinning, vertex-coloured triangles with vsync off
 * for ~5 seconds and prints the average frame rate. Run it once with the
 * system driver (llvmpipe / software) and once with grate-mesa to compare
 * the Tegra30 GR3D against software rendering. See run_bench.sh.
 *
 * Build on the device:
 *   apk add build-base mesa-dev libx11-dev
 *   gcc -O2 -o gltri_bench gltri_bench.c -lEGL -lGLESv2 -lX11 -lm
 */
#define _POSIX_C_SOURCE 199309L
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <time.h>
#include <X11/Xlib.h>
#include <EGL/egl.h>
#include <GLES2/gl2.h>

/* The rotation is fed in as (cos a, sin a): grate-mesa's vertex-shader
 * compiler has no SIN/COS opcode, so the trig stays on the CPU and the
 * shader is pure ALU - exactly what the Tegra GR3D wants. */
static const char *vs_src =
   "attribute vec2 pos;\n"
   "attribute vec3 col;\n"
   "uniform vec2 rot;\n"
   "varying vec3 vcol;\n"
   "void main() {\n"
   "   gl_Position = vec4(pos.x*rot.x - pos.y*rot.y,\n"
   "                      pos.x*rot.y + pos.y*rot.x, 0.0, 1.0);\n"
   "   vcol = col;\n"
   "}\n";

static const char *fs_src =
   "precision mediump float;\n"
   "varying vec3 vcol;\n"
   "void main() { gl_FragColor = vec4(vcol, 1.0); }\n";

static GLuint compile_shader(GLenum type, const char *src)
{
   GLuint s = glCreateShader(type);
   GLint ok = 0;
   glShaderSource(s, 1, &src, NULL);
   glCompileShader(s);
   glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
   if (!ok) {
      char log[512];
      glGetShaderInfoLog(s, sizeof log, NULL, log);
      fprintf(stderr, "shader compile failed: %s\n", log);
      exit(1);
   }
   return s;
}

static double now_sec(void)
{
   struct timespec t;
   clock_gettime(CLOCK_MONOTONIC, &t);
   return t.tv_sec + t.tv_nsec / 1e9;
}

int main(int argc, char **argv)
{
   int tris = (argc > 1) ? atoi(argv[1]) : 20000;
   const int W = 256, H = 256;
   const double run = 5.0;
   int i, v;

   if (tris < 1)
      tris = 1;

   Display *xdpy = XOpenDisplay(NULL);
   if (!xdpy) {
      fprintf(stderr, "cannot open X display\n");
      return 1;
   }
   XSetWindowAttributes swa;
   swa.event_mask = ExposureMask;
   Window win = XCreateWindow(xdpy, DefaultRootWindow(xdpy), 0, 0, W, H, 0,
                              CopyFromParent, InputOutput, CopyFromParent,
                              CWEventMask, &swa);
   XStoreName(xdpy, win, "gltri_bench");
   XMapWindow(xdpy, win);

   EGLDisplay edpy = eglGetDisplay((EGLNativeDisplayType)xdpy);
   eglInitialize(edpy, NULL, NULL);
   eglBindAPI(EGL_OPENGL_ES_API);

   EGLint cfg_attr[] = {
      EGL_RED_SIZE, 1, EGL_GREEN_SIZE, 1, EGL_BLUE_SIZE, 1,
      EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
      EGL_SURFACE_TYPE, EGL_WINDOW_BIT, EGL_NONE
   };
   EGLConfig cfg;
   EGLint ncfg = 0;
   if (!eglChooseConfig(edpy, cfg_attr, &cfg, 1, &ncfg) || ncfg < 1) {
      fprintf(stderr, "no usable EGL config\n");
      return 1;
   }
   EGLSurface surf = eglCreateWindowSurface(edpy, cfg,
                                            (EGLNativeWindowType)win, NULL);
   EGLint ctx_attr[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
   EGLContext ctx = eglCreateContext(edpy, cfg, EGL_NO_CONTEXT, ctx_attr);
   if (!eglMakeCurrent(edpy, surf, surf, ctx)) {
      fprintf(stderr, "eglMakeCurrent failed\n");
      return 1;
   }
   eglSwapInterval(edpy, 0);   /* vsync off - measure real throughput */

   printf("gltri_bench: %d triangles, %dx%d, %.0fs run\n", tris, W, H, run);
   printf("GL_RENDERER : %s\n", (const char *)glGetString(GL_RENDERER));

   GLuint prog = glCreateProgram();
   glAttachShader(prog, compile_shader(GL_VERTEX_SHADER, vs_src));
   glAttachShader(prog, compile_shader(GL_FRAGMENT_SHADER, fs_src));
   glBindAttribLocation(prog, 0, "pos");
   glBindAttribLocation(prog, 1, "col");
   glLinkProgram(prog);
   glUseProgram(prog);
   GLint u_rot = glGetUniformLocation(prog, "rot");

   int nverts = tris * 3;
   float *pos = malloc((size_t)nverts * 2 * sizeof(float));
   float *col = malloc((size_t)nverts * 3 * sizeof(float));
   if (!pos || !col) {
      fprintf(stderr, "out of memory\n");
      return 1;
   }
   srand(1);
   for (i = 0; i < tris; i++) {
      float cx = rand() / (float)RAND_MAX * 2.0f - 1.0f;
      float cy = rand() / (float)RAND_MAX * 2.0f - 1.0f;
      for (v = 0; v < 3; v++) {
         int k = i * 3 + v;
         float a = v * 2.0943951f;          /* 120 degrees */
         pos[k * 2 + 0] = cx + 0.05f * cosf(a);
         pos[k * 2 + 1] = cy + 0.05f * sinf(a);
         col[k * 3 + 0] = (v == 0) ? 1.0f : 0.0f;
         col[k * 3 + 1] = (v == 1) ? 1.0f : 0.0f;
         col[k * 3 + 2] = (v == 2) ? 1.0f : 0.0f;
      }
   }

   GLuint vbo[2];
   glGenBuffers(2, vbo);
   glBindBuffer(GL_ARRAY_BUFFER, vbo[0]);
   glBufferData(GL_ARRAY_BUFFER, (size_t)nverts * 2 * sizeof(float), pos,
                GL_STATIC_DRAW);
   glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, 0);
   glEnableVertexAttribArray(0);
   glBindBuffer(GL_ARRAY_BUFFER, vbo[1]);
   glBufferData(GL_ARRAY_BUFFER, (size_t)nverts * 3 * sizeof(float), col,
                GL_STATIC_DRAW);
   glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 0, 0);
   glEnableVertexAttribArray(1);

   glViewport(0, 0, W, H);
   glClearColor(0.1f, 0.1f, 0.1f, 1.0f);

   long frames = 0;
   double t0 = now_sec(), t;
   while ((t = now_sec()) - t0 < run) {
      float a = (float)(t - t0);
      glClear(GL_COLOR_BUFFER_BIT);
      glUniform2f(u_rot, cosf(a), sinf(a));
      glDrawArrays(GL_TRIANGLES, 0, nverts);
      eglSwapBuffers(edpy, surf);
      frames++;
   }
   glFinish();
   double elapsed = now_sec() - t0;

   printf("RESULT: %ld frames in %.2fs = %.1f FPS\n",
          frames, elapsed, frames / elapsed);

   eglMakeCurrent(edpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
   eglDestroyContext(edpy, ctx);
   eglDestroySurface(edpy, surf);
   eglTerminate(edpy);
   XDestroyWindow(xdpy, win);
   XCloseDisplay(xdpy);
   return 0;
}

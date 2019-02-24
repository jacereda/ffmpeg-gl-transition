/**
 * FFmpeg filter for applying GLSL transitions between video streams.
 *
 * @see https://gl-transitions.com/
 */

#include "libavutil/opt.h"
#include "libavutil/avstring.h"
#include "internal.h"
#include "framesync.h"

#ifndef __APPLE__
# define GL_TRANSITION_USING_EGL //remove this line if you don't want to use EGL
#endif

#ifdef __APPLE__
# define __gl_h_
# define GL_DO_NOT_WARN_IF_MULTI_GL_VERSION_HEADERS_INCLUDED
# include <OpenGL/gl3.h>
#else
# include <GL/glew.h>
#endif

#ifdef GL_TRANSITION_USING_EGL
# include <EGL/egl.h>
#else
# include <GLFW/glfw3.h>
#endif

#include <stdio.h>
#include <stdlib.h>
#include <float.h>

#define FROM (0)
#define TO   (1)

#define PIXEL_FORMAT (GL_RGB)

#ifdef GL_TRANSITION_USING_EGL
static const EGLint configAttribs[] = {
    EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
    EGL_BLUE_SIZE, 8,
    EGL_GREEN_SIZE, 8,
    EGL_RED_SIZE, 8,
    EGL_DEPTH_SIZE, 8,
    EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT,
    EGL_NONE};
#endif
static const float position[12] = {
  -1.0f, -1.0f, 1.0f, -1.0f, -1.0f, 1.0f, -1.0f, 1.0f, 1.0f, -1.0f, 1.0f, 1.0f
};

static const GLchar *v_shader_source =
  "attribute vec2 position;\n"
  "varying vec2 _uv;\n"
  "void main(void) {\n"
  "  gl_Position = vec4(position, 0, 1);\n"
  "  vec2 uv = position * 0.5 + 0.5;\n"
  "  _uv = vec2(uv.x, 1.0 - uv.y);\n"
  "}\n";

static const GLchar *f_shader_template =
  "varying vec2 _uv;\n"
  "uniform sampler2D from;\n"
  "uniform sampler2D to;\n"
  "uniform float progress;\n"
  "uniform float ratio;\n"
  "uniform mat3 mfrom;\n"
  "uniform mat3 mto;\n"    
  "\n"
  "vec4 getFromColor(vec2 uv) {\n"
  "  return texture2D(from, vec2(vec3(uv,1.) * mfrom));\n"
  "}\n"
  "\n"
  "vec4 getToColor(vec2 uv) {\n"
  "  return texture2D(to, vec2(vec3(uv,1.) * mto));\n"
  "}\n"
  "\n"
  "#line 0 0\n"
  "\n%s\n"
  "void main() {\n"
  "  gl_FragColor = transition(_uv);\n"
  "}\n";

// default to a basic fade effect
static const GLchar *f_default_transition_source =
  "vec4 transition (vec2 uv) {\n"
  "  return mix(\n"
  "    getFromColor(uv),\n"
  "    getToColor(uv),\n"
  "    progress\n"
  "  );\n"
  "}\n";

enum ResizeType { CONTAIN, COVER, STRETCH, RESIZES_NB };

typedef struct {
  const AVClass *class;
  FFFrameSync fs;

  // input options
  double duration;
  double offset;
  ResizeType resize;
  
  char *source;

  // output options
  unsigned w, h;
  
  // timestamp of the first frame in the output, in the timebase units
  int64_t first_pts;

  // uniforms
  GLuint        from;
  GLuint        to;
  GLint         progress;

  // internal state
  GLuint        posBuf;
  GLuint        program;
#ifdef GL_TRANSITION_USING_EGL
  EGLDisplay eglDpy;
  EGLConfig eglCfg;
  EGLSurface eglSurf;
  EGLContext eglCtx;
#else
  GLFWwindow    *window;
#endif

  GLchar *f_shader_source;
} GLTransitionContext;

#define OFFSET(x) offsetof(GLTransitionContext, x)
#define FLAGS AV_OPT_FLAG_FILTERING_PARAM|AV_OPT_FLAG_VIDEO_PARAM

static const AVOption gltransition_options[] = {
  { "duration", "transition duration in seconds", OFFSET(duration), AV_OPT_TYPE_DOUBLE, {.dbl=1.0}, 0, DBL_MAX, FLAGS },
  { "offset", "delay before startingtransition in seconds", OFFSET(offset), AV_OPT_TYPE_DOUBLE, {.dbl=0.0}, 0, DBL_MAX, FLAGS },
  { "source", "path to the gl-transition source file (defaults to basic fade)", OFFSET(source), AV_OPT_TYPE_STRING, {.str = NULL}, CHAR_MIN, CHAR_MAX, FLAGS },
  { "w", "Output video width", OFFSET(w),    AV_OPT_TYPE_INT, {.i64=0}, 0,8192, FLAGS },
  { "h", "Output video height", OFFSET(h),    AV_OPT_TYPE_INT, {.i64=0}, 0,8192, FLAGS },
  { "resize", "resize mode", OFFSET(resize), AV_OPT_TYPE_INT, {.i64=0}, 0, RESIZE_NB-1, FLAGS, "resize" },
  { "contain", "contain", 0, AV_OPT_TYPE_CONST, {.i64=CONTAIN}, 0, 0, FLAGS, "resize" },
  { "cover", "cover", 0, AV_OPT_TYPE_CONST, {.i64=COVER}, 0, 0, FLAGS, "resize" },
  { "stretch", "stretch", 0, AV_OPT_TYPE_CONST, {.i64=STRETCH}, 0, 0, FLAGS, "resize" },  
  {NULL}
};

FRAMESYNC_DEFINE_CLASS(gltransition, GLTransitionContext, fs);

static GLuint build_shader(AVFilterContext *ctx, const GLchar *shader_source, GLenum type)
{
  GLint status;
  GLuint shader = glCreateShader(type);
  if (!shader || !glIsShader(shader)) {
    return 0;
  }

  glShaderSource(shader, 1, &shader_source, 0);
  glCompileShader(shader);

  glGetShaderiv(shader, GL_COMPILE_STATUS, &status);

  if (status != GL_TRUE) {
    char log[10000];
    glGetShaderInfoLog(shader, sizeof(log), NULL, log);
    av_log(ctx, AV_LOG_ERROR, "invalid shader: %s\n", log);
  }

  return (status == GL_TRUE ? shader : 0);
}

static int build_program(AVFilterContext *ctx)
{
  GLint status;
  GLuint v_shader, f_shader;
  GLTransitionContext *c = ctx->priv;
  char *source = NULL;
  const char * transition_source;
  int len;

  if (!(v_shader = build_shader(ctx, v_shader_source, GL_VERTEX_SHADER))) {
    return -1;
  }


  if (c->source) {
    FILE *f = fopen(c->source, "rb");
    unsigned long fsize;
    
    if (!f) {
      av_log(ctx, AV_LOG_ERROR, "invalid transition source file \"%s\"\n", c->source);
      return -1;
    }

    fseek(f, 0, SEEK_END);
    fsize = ftell(f);
    fseek(f, 0, SEEK_SET);

    source = malloc(fsize + 1);
    fread(source, fsize, 1, f);
    fclose(f);

    source[fsize] = 0;
  }

  transition_source = source ? source : f_default_transition_source;

  len = strlen(f_shader_template) + strlen(transition_source);
  c->f_shader_source = av_calloc(len, sizeof(*c->f_shader_source));
  if (!c->f_shader_source) {
    return AVERROR(ENOMEM);
  }

  snprintf(c->f_shader_source, len * sizeof(*c->f_shader_source), f_shader_template, transition_source);
  av_log(ctx, AV_LOG_DEBUG, "\n%s\n", c->f_shader_source);

  if (source) {
    free(source);
    source = NULL;
  }

  if (!(f_shader = build_shader(ctx, c->f_shader_source, GL_FRAGMENT_SHADER))) {
    return -1;
  }
  
  c->program = glCreateProgram();
  glAttachShader(c->program, v_shader);
  glAttachShader(c->program, f_shader);
  glLinkProgram(c->program);

  glGetProgramiv(c->program, GL_LINK_STATUS, &status);
  if (status != GL_TRUE) {
    char log[10000];
    glGetProgramInfoLog(c->program, sizeof(log), NULL, log);
    av_log(ctx, AV_LOG_ERROR, "invalid program: %s\n", log);
  }
  return status == GL_TRUE ? 0 : -1;
}

static GLuint create_vbo(GLTransitionContext *c)
{
  GLint loc;
  GLuint buf;
  glGenBuffers(1, &buf);
  glBindBuffer(GL_ARRAY_BUFFER, buf);
  glBufferData(GL_ARRAY_BUFFER, sizeof(position), position, GL_STATIC_DRAW);

  loc = glGetAttribLocation(c->program, "position");
  glEnableVertexAttribArray(loc);
  glVertexAttribPointer(loc, 2, GL_FLOAT, GL_FALSE, 0, 0);
  return buf;
}

static GLuint create_tex(unsigned w, unsigned h) {
  GLuint t;
  glGenTextures(1, &t);
  glActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_2D, t);

  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_BORDER);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_BORDER);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);

  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, w, h, 0, PIXEL_FORMAT, GL_UNSIGNED_BYTE, NULL);
  return t;
}

static int streq(const char * s1, const char * s2) {
  return s1 && s2 && !strcmp(s1,s2);
}

static void get_matrix(int method, float * m, float ratio, float xratio) {
  float sx, sy;
  memset(m, 0, 9*sizeof(float));
  switch (method) {
  case CONTAIN: 
    sx = fmax(ratio/xratio, 1.f);
    sy = fmax(xratio/ratio, 1.f); 
    break;
  case COVER:
    sx = fmin(ratio/xratio, 1.f);
    sy = fmin(xratio/ratio, 1.f);
    break;
  case STRETCH:
    sx = 1;
    sy = 1;
    break;
  }
  m[0] = sx;
  m[2] = -0.5 * sx + 0.5;
  m[4] = -sy;
  m[5] = 0.5 * sy + 0.5;
  m[8] = 1;
}

static void init_uniforms(AVFilterContext * ctx)
{
  GLTransitionContext *c = ctx->priv;
  AVFilterLink *fromLink = ctx->inputs[FROM];
  AVFilterLink *toLink = ctx->inputs[TO];
  AVFilterLink *outLink = ctx->outputs[0];
  float mfrom[9];
  float mto[9];
  float ratio = outLink->w / (float)outLink->h;
  float fromR = fromLink->w / (float)fromLink->h;
  float toR = toLink->w / (float)toLink->h;  
  char * src = strdup(c->f_shader_source);
  char * st;
  char * line;
  line = strtok_r(src, "\r\n", &st);  
  while (line) {
#define WHITE " \t"
    char * ist;
    char * start = strtok_r(line, WHITE, &ist);
    char * type = strtok_r(NULL, WHITE, &ist);
    char * name = strtok_r(NULL, WHITE ";", &ist); 
    char * comm = strtok_r(NULL, WHITE, &ist);
    char * eq = strtok_r(NULL, WHITE, &ist);
    char * val = strtok_r(NULL, WHITE ";", &ist);
    (void)comm;
    (void)start;
    av_log(ctx, AV_LOG_DEBUG, "start %s type %s name %s comm %s eq %s val %s\n", start, type, name, comm, eq, val);
    if (streq(start, "uniform") && val) {
      GLint loc;
      loc = glGetUniformLocation(c->program, name);
      if (loc < 0)
	  av_log(ctx, AV_LOG_ERROR, "no uniform named %s\n", name);
      else if (streq(type, "bool")) {
	if (av_match_name(val, "true,1"))
	  glUniform1i(loc, 1);
	else if (av_match_name(val, "false,0"))
	  glUniform1i(loc, 0);
	else
	  av_log(ctx, AV_LOG_ERROR, "parsing bool %s for uniform %s\n", val, name);
      }
      else if (streq(type, "int")) {
	int v;
	if (1 == sscanf(val, "%d", &v))
	  glUniform1i(loc, v);
	else
	  av_log(ctx, AV_LOG_ERROR, "parsing integer %s for uniform %s\n", val, name);
      }
      else if (streq(type, "float")) {
	float v;
	if (1 == sscanf(val, "%f", &v))
	  glUniform1f(loc, v);
	else
	  av_log(ctx, AV_LOG_ERROR, "parsing float %s for uniform %s\n", val, name);
      }
      else if (streq(type, "ivec2")) {
	int x,y;
	if (2 == sscanf(val, "ivec2(%d,%d)", &x, &y))
	  glUniform2i(loc, x, x);
	else if (1 == sscanf(val, "ivec2(%d)", &x))
	  glUniform2i(loc, x, x);
	else
	  av_log(ctx, AV_LOG_ERROR, "parsing ivec2 %s for uniform %s\n", val, name);
      }
      else if (streq(type, "vec2")) {
	float x,y;
	if (2 == sscanf(val, "vec2(%f,%f)", &x, &y))
	  glUniform2f(loc, x, x);
	else if (1 == sscanf(val, "vec2(%f)", &x))
	  glUniform2f(loc, x, x);
	else
	  av_log(ctx, AV_LOG_ERROR, "parsing vec2 %s for uniform %s\n", val, name);
      }
      else if (streq(type, "vec3")) {
	float x,y,z;
	if (3 == sscanf(val, "vec3(%f,%f,%f)", &x, &y, &z))
	  glUniform3f(loc, x, y, z);
	else if (1 == sscanf(val, "vec3(%f)", &x))
	  glUniform3f(loc, x, x, x);
	else
	  av_log(ctx, AV_LOG_ERROR, "parsing vec3 %s for uniform %s\n", val, name);
      }
      else if (streq(type, "vec4")) {
	float x,y,z,w;
	if (4 == sscanf(val, "vec4(%f,%f,%f,%f)", &x, &y, &z, &w))
	  glUniform4f(loc, x, y, z, w);
	else if (1 == sscanf(val, "vec4(%f)", &x))
	  glUniform4f(loc, x, x, x, x);
	else
	  av_log(ctx, AV_LOG_ERROR, "parsing vec4 %s for uniform %s\n", val, name);
      }
      else
	av_log(ctx, AV_LOG_ERROR, "unrecognized type %s for uniform %s\n", type, name);
    }
    line = strtok_r(NULL, "\r\n", &st);
  }
  free(src);

  glUniform1i(glGetUniformLocation(c->program, "from"), 0);
  glUniform1i(glGetUniformLocation(c->program, "to"), 1);
  
  c->progress = glGetUniformLocation(c->program, "progress");
  glUniform1f(c->progress, 0.0f);

  glUniform1f(glGetUniformLocation(c->program, "ratio"), ratio);

  get_matrix(c->resize, mfrom, ratio, fromR);
  glUniformMatrix3fv(glGetUniformLocation(c->program, "mfrom"), 1, GL_FALSE, mfrom);
  get_matrix(c->resize, mto, ratio, toR);    
  glUniformMatrix3fv(glGetUniformLocation(c->program, "mto"), 1, GL_FALSE, mto);  
}

static AVFrame *apply_transition(FFFrameSync *fs,
                                 AVFilterContext *ctx,
                                 AVFrame *fromFrame,
                                 const AVFrame *toFrame)
{
  GLTransitionContext *c = ctx->priv;
  AVFilterLink *fromLink = ctx->inputs[FROM];
  AVFilterLink *toLink = ctx->inputs[TO];
  AVFilterLink *outLink = ctx->outputs[0];
  AVFrame *outFrame;
  float ts;
  float progress;

  outFrame = ff_get_video_buffer(outLink, outLink->w, outLink->h);
  if (!outFrame) {
    return NULL;
  }

  av_frame_copy_props(outFrame, fromFrame);

#ifdef GL_TRANSITION_USING_EGL
  eglMakeCurrent(c->eglDpy, c->eglSurf, c->eglSurf, c->eglCtx);
#else
  glfwMakeContextCurrent(c->window);
#endif

  glUseProgram(c->program);

  ts = ((fs->pts - c->first_pts) / (float)fs->time_base.den) - c->offset;
  progress = FFMAX(0.0f, FFMIN(1.0f, ts / c->duration));
  // av_log(ctx, AV_LOG_ERROR, "transition '%s' %llu %f %f\n", c->source, fs->pts - c->first_pts, ts, progress);
  glUniform1f(c->progress, progress);

  glActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_2D, c->from);
  glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
  glPixelStorei(GL_UNPACK_ROW_LENGTH, fromFrame->linesize[0]/3);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, fromLink->w, fromLink->h, 0, PIXEL_FORMAT, GL_UNSIGNED_BYTE, fromFrame->data[0]);
  
  glActiveTexture(GL_TEXTURE0 + 1);
  glBindTexture(GL_TEXTURE_2D, c->to);
  glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
  glPixelStorei(GL_UNPACK_ROW_LENGTH, toFrame->linesize[0]/3);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, toLink->w, toLink->h, 0, PIXEL_FORMAT, GL_UNSIGNED_BYTE, toFrame->data[0]);

  glDrawArrays(GL_TRIANGLES, 0, 6);
  glPixelStorei(GL_PACK_ALIGNMENT, 1);
  glPixelStorei(GL_PACK_ROW_LENGTH, outFrame->linesize[0]/3);
  glReadPixels(0, 0, outLink->w, outLink->h, PIXEL_FORMAT, GL_UNSIGNED_BYTE, outFrame->data[0]);


  av_log(ctx, AV_LOG_DEBUG, "linesize %d %d %d\n", fromFrame->linesize[0], toFrame->linesize[0], outFrame->linesize[0]);
  av_log(ctx, AV_LOG_DEBUG, "frame: %dx%d %dx%d %dx%d\n", fromLink->w, fromLink->h, toLink->w, toLink->h, outLink->w, outLink->h);

  av_log(ctx, AV_LOG_DEBUG, "frame2: %dx%d %dx%d %dx%d\n", fromFrame->width, fromFrame->height, toFrame->width, toFrame->height, outFrame->width, outFrame->height);  
  
  av_frame_free(&fromFrame);

  return outFrame;
}

static int blend_frame(FFFrameSync *fs)
{
  AVFilterContext *ctx = fs->parent;
  GLTransitionContext *c = ctx->priv;

  AVFrame *fromFrame, *toFrame, *outFrame;
  int ret;

  ret = ff_framesync_dualinput_get(fs, &fromFrame, &toFrame);
  if (ret < 0) {
    return ret;
  }

  if (c->first_pts == AV_NOPTS_VALUE && fromFrame && fromFrame->pts != AV_NOPTS_VALUE) {
    c->first_pts = fromFrame->pts;
  }

  if (!toFrame) {
    return ff_filter_frame(ctx->outputs[0], fromFrame);
  }

  outFrame = apply_transition(fs, ctx, fromFrame, toFrame);
  if (!outFrame) {
    return AVERROR(ENOMEM);
  }

  return ff_filter_frame(ctx->outputs[0], outFrame);
}

static av_cold int init(AVFilterContext *ctx)
{
  GLTransitionContext *c = ctx->priv;
  c->fs.on_event = blend_frame;
  c->first_pts = AV_NOPTS_VALUE;

  return 0;
}

static av_cold void uninit(AVFilterContext *ctx) {
  GLTransitionContext *c = ctx->priv;
  ff_framesync_uninit(&c->fs);
  
  if (c->from)
    glDeleteTextures(1, &c->from);
  if (c->to)
    glDeleteTextures(1, &c->to);
  if (c->posBuf)
    glDeleteBuffers(1, &c->posBuf);
  if (c->program)
    glDeleteProgram(c->program);
  
#ifdef GL_TRANSITION_USING_EGL
  if (c->eglDpy) {
    eglTerminate(c->eglDpy);
  }
#else
  if (c->window) {
    glfwDestroyWindow(c->window);
  }
#endif

  if (c->f_shader_source) {
    av_freep(&c->f_shader_source);
  }
}

static int query_formats(AVFilterContext *ctx)
{
  static const enum AVPixelFormat formats[] = {
    AV_PIX_FMT_RGB24,
    AV_PIX_FMT_NONE
  };

  return ff_set_common_formats(ctx, ff_make_format_list(formats));
}

static int activate(AVFilterContext *ctx)
{
  GLTransitionContext *c = ctx->priv;
  return ff_framesync_activate(&c->fs);
}

static int config_output(AVFilterLink *outLink)
{
  AVFilterContext *ctx = outLink->src;
  GLTransitionContext *c = ctx->priv;
  AVFilterLink *fromLink = ctx->inputs[FROM];
  AVFilterLink *toLink = ctx->inputs[TO];
  int ret;

  if (fromLink->format != toLink->format) {
    av_log(ctx, AV_LOG_ERROR, "inputs must be of same pixel format\n");
    return AVERROR(EINVAL);
  }

  if (c->w <= 0 || c->h <= 0) {
    av_log(ctx, AV_LOG_ERROR, "width and height parameters must be set\n");
    return AVERROR(EINVAL);
  }

  outLink->w = c->w;
  outLink->h = c->h;
  // outLink->time_base = fromLink->time_base;
  outLink->frame_rate = fromLink->frame_rate;

#ifdef GL_TRANSITION_USING_EGL
  //init EGL
  // 1. Initialize EGL
  c->eglDpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
  EGLint major, minor;
  eglInitialize(c->eglDpy, &major, &minor);
  av_log(ctx, AV_LOG_DEBUG, "%d%d", major, minor);
  
  // 2. Select an appropriate configuration  
  EGLint numConfigs;
  EGLint pbufferAttribs[] = {
      EGL_WIDTH, outLink->w,
      EGL_HEIGHT, outLink->h,
      EGL_NONE,
  };
  eglChooseConfig(c->eglDpy, configAttribs, &c->eglCfg, 1, &numConfigs);
  // 3. Create a surface
  c->eglSurf = eglCreatePbufferSurface(c->eglDpy, c->eglCfg,
                                       pbufferAttribs);
  // 4. Bind the API
  eglBindAPI(EGL_OPENGL_API);
  // 5. Create a context and make it current
  c->eglCtx = eglCreateContext(c->eglDpy, c->eglCfg, EGL_NO_CONTEXT, NULL);
  eglMakeCurrent(c->eglDpy, c->eglSurf, c->eglSurf, c->eglCtx);
#else
  //glfw
  if (!glfwInit())
  {
    return -1;
  }
  glfwWindowHint(GLFW_VISIBLE, 0);

  c->window = glfwCreateWindow(outLink->w, outLink->h, "", NULL, NULL);
  if (!c->window) {
    av_log(ctx, AV_LOG_ERROR, "setup_gl ERROR\n");
    return -1;
  }
  glfwMakeContextCurrent(c->window);

#endif

#ifndef __APPLE__
  glewExperimental = GL_TRUE;
  glewInit();
#endif

  glViewport(0, 0, outLink->w, outLink->h);
  
  if((ret = build_program(ctx)) < 0) {
    return ret;
  }
  glUseProgram(c->program);
  c->posBuf = create_vbo(c);
  init_uniforms(ctx);

  c->from = create_tex(fromLink->w, fromLink->h);
  c->to = create_tex(toLink->w, toLink->h);  

  if ((ret = ff_framesync_init_dualinput(&c->fs, ctx)) < 0) {
    return ret;
  }
  av_log(ctx, AV_LOG_DEBUG, "ok: %d %d %dx%d %dx%d %dx%d\n", c->from, c->to, fromLink->w, fromLink->h, toLink->w, toLink->h, outLink->w, outLink->h);
  
  return ff_framesync_configure(&c->fs);
}

static const AVFilterPad gltransition_inputs[] = {
  {
    .name = "from",
    .type = AVMEDIA_TYPE_VIDEO,
  },
  {
    .name = "to",
    .type = AVMEDIA_TYPE_VIDEO,
  },
  {NULL}
};

static const AVFilterPad gltransition_outputs[] = {
  {
    .name = "default",
    .type = AVMEDIA_TYPE_VIDEO,
    .config_props = config_output,
  },
  {NULL}
};

AVFilter ff_vf_gltransition = {
  .name          = "gltransition",
  .description   = NULL_IF_CONFIG_SMALL("OpenGL blend transitions"),
  .priv_size     = sizeof(GLTransitionContext),
  .preinit       = gltransition_framesync_preinit,
  .init          = init,
  .uninit        = uninit,
  .query_formats = query_formats,
  .activate      = activate,
  .inputs        = gltransition_inputs,
  .outputs       = gltransition_outputs,
  .priv_class    = &gltransition_class,
  .flags         = AVFILTER_FLAG_SUPPORT_TIMELINE_GENERIC
};

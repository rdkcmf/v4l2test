/*
 * If not stated otherwise in this file or this component's Licenses.txt file the
 * following copyright and licenses apply:
 *
 * Copyright 2019 RDK Management
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <stdarg.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <signal.h>
#include <sys/file.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <dirent.h>
#include <unistd.h>

#include <linux/videodev2.h>

#include <drm/drm_fourcc.h>

#define EGL_EGLEXT_PROTOTYPES
#include <EGL/egl.h>
#include <EGL/eglext.h>

#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h> 

#include "platform.h"

#define DEFAULT_WIDTH (1920)
#define DEFAULT_HEIGHT (1080)
#define DEFAULT_FRAME_WIDTH (1920)
#define DEFAULT_FRAME_HEIGHT (800)
#define DEFAULT_FRAME_RATE (24)

#define NUM_INPUT_BUFFERS (2)
#define MIN_INPUT_BUFFERS (1)
#define NUM_OUTPUT_BUFFERS (6)
#define MIN_OUTPUT_BUFFERS (3)

#define MAX_TEXTURES (2)

typedef struct _AppCtx AppCtx;
typedef struct _DecCtx DecCtx;

#define IOCTL ioctl_wrapper

typedef struct _EGLCtx
{
   AppCtx *appCtx;
   bool initialized;
   bool useWayland;
   void *nativeDisplay;
   struct wl_display *dispWayland;
   EGLDisplay eglDisplay;
   EGLContext eglContext;   
   EGLSurface eglSurface;
   EGLConfig eglConfig;
   EGLint majorVersion;
   EGLint minorVersion;
   void *nativeWindow;
} EGLCtx;

typedef struct _GLCtx
{
   AppCtx *appCtx;
   bool initialized;

   PFNEGLCREATEIMAGEKHRPROC eglCreateImageKHR;
   PFNEGLDESTROYIMAGEKHRPROC eglDestroyImageKHR;
   PFNGLEGLIMAGETARGETTEXTURE2DOESPROC glEGLImageTargetTexture2DOES;

   GLuint fragColor;
   GLuint vertColor;
   GLuint progColor;
   GLint locPosColor;
   GLint locColorColor;
   GLint locOffsetColor;
   GLint locMatrixColor;

   bool haveYUVTextures;
   bool haveYUVShaders;
   GLuint fragTex;
   GLuint vertTex;
   GLuint progTex;
   GLint locPosTex;
   GLint locTC;
   GLint locTCUV;
   GLint locResTex;
   GLint locMatrixTex;
   GLint locTexture;
   GLint locTextureUV;

   GLuint fragFill;
   GLuint vertFill;
   GLuint progFill;
   GLint locPosFill;
   GLint locResFill;
   GLint locMatrixFill;
   GLint locColorFill;
} GLCtx;

typedef struct _Surface
{
   int x;
   int y;
   int w;
   int h;
   bool dirty;
   bool haveYUVTextures;
   bool externalImage;
   int textureCount;
   GLuint textureId[MAX_TEXTURES];
   EGLImageKHR eglImage[MAX_TEXTURES];
} Surface;

typedef struct _PlaneInfo
{
   int fd;
   void *start;
   int capacity;
} PlaneInfo;

#define MAX_PLANES (3)
typedef struct _BufferInfo
{
   struct v4l2_buffer buf;
   struct v4l2_plane planes[MAX_PLANES];
   PlaneInfo planeInfo[MAX_PLANES];
   int planeCount;
   int fd;
   void *start;
   int capacity;
} BufferInfo;

typedef struct _V4l2Ctx
{
   DecCtx *decCtx;
   int v4l2Fd;
   struct v4l2_capability caps;
   uint32_t deviceCaps;
   bool isMultiPlane;
   int numInputFormats;
   struct v4l2_fmtdesc *inputFormats;
   int numOutputFormats;
   struct v4l2_fmtdesc *outputFormats;
   struct v4l2_format fmtIn;
   struct v4l2_format fmtOut;
   uint32_t minBuffersIn;
   uint32_t minBuffersOut;
   int numBuffersIn;
   BufferInfo *inBuffers;
   int numBuffersOut;
   BufferInfo *outBuffers;
   uint32_t inputFormat;
   bool outputStarted;
} V4l2Ctx;

typedef struct _Async
{
   bool started;
   bool error;
   bool done;
} Async;

#define MAX_STREAM_LEN (40000000)
#define MAX_STREAM_FRAMES (2000)
typedef struct _Stream
{
   char *inputFilename;
   int streamDataLen;
   char *streamData;
   int streamFrameCount;
   int streamFrameOffset[MAX_STREAM_FRAMES];
   int streamFrameLength[MAX_STREAM_FRAMES];
   int videoWidth;
   int videoHeight;
   int videoRate;
} Stream;

typedef struct _DecCtx
{
   AppCtx *appCtx;

   pthread_mutex_t mutex;

   int videoWidth;
   int videoHeight;
   int videoRate;

   int prevFrameFd;
   int currFrameFd;
   int nextFrameFd;
   int nextFrameFd1;

   V4l2Ctx v4l2;
   bool playing;
   bool paused;
   bool ready;
   int outputFrameCount;
   long long startTime;
   long long stopTime;
   int numFramesToDecode;
   int decodeIndex;

   Surface *surface;
   Async *async;
   Stream *stream;

   pthread_t videoInThreadId;
   bool videoInThreadStarted;
   bool videoInThreadStopRequested;

   pthread_t videoOutThreadId;
   bool videoOutThreadStarted;
   bool videoOutThreadStopRequested;

   pthread_t videoEOSThreadId;
   bool videoEOSThreadStarted;
   bool videoEOSThreadStopRequested;

   pthread_t videoDecodeThreadId;
   bool videoDecodeThreadStarted;
   bool videoDecodeThreadStopRequested;
} DecCtx;

#define NUM_DECODE (4)

typedef struct _AppCtx
{
   PlatformCtx *platformCtx;
   EGLCtx egl;
   GLCtx gl;
   bool haveDmaBufImport;
   bool haveExternalImage;

   int windowWidth;
   int windowHeight;

   int videoWidth;
   int videoHeight;
   int videoRate;

   int numFramesToDecode;

   DecCtx decode[NUM_DECODE];
   Surface surface[NUM_DECODE];
   Async async[NUM_DECODE];
   Stream stream[NUM_DECODE];

} AppCtx;

static bool gVerbose= false;
static int gLogLevel= 0;
static char *gDeviceName= 0;
static FILE *gReport= 0;

static void iprintf( int level, const char *fmt, ... );
static long long getCurrentTimeMillis(void);
static double getCpuIdle();
static void emitLoadAverage();
static bool initEGL( EGLCtx *eglCtx );
static void termEGL( EGLCtx *eglCtx );
static bool initGL( GLCtx *ctx );
static void termGL( GLCtx *ctx );
static void drawSurface( GLCtx *glCtx, Surface *surface );
static int ioctl_wrapper( int fd, int request, void* arg );
static bool getInputFormats( V4l2Ctx *v4l2 );
static bool getOutputFormats( V4l2Ctx *v4l2 );
static bool setInputFormat( V4l2Ctx *v4l2 );
static bool setOutputFormat( V4l2Ctx *v4l2 );
static bool setupInputBuffers( V4l2Ctx *v4l2 );
static void tearDownInputBuffers( V4l2Ctx *v4l2 );
static bool setupOutputBuffers( V4l2Ctx *v4l2 );
static void tearDownOutputBuffers( V4l2Ctx *v4l2 );
static void stopDecoder( V4l2Ctx *v4l2 );
static bool initV4l2( V4l2Ctx *v4l2 );
static void termV4l2( V4l2Ctx *v4l2 );
static int getInputBuffer( V4l2Ctx *v4l2 );
static int getOutputBuffer( V4l2Ctx *v4l2 );
static int findOutputBuffer( V4l2Ctx *v4l2, int fd );
static void *videoEOSThread( void *arg );
static void *videoOutputThread( void *arg );
static void *videoInputThread( void *arg );
static void *videoDecodeThread( void *arg );
static bool playFile( DecCtx *decCtx );
static bool parseStreamDescriptor( AppCtx *appCtx, Stream *stream, const char *descriptorFilename );
static bool prepareStream( AppCtx *appCtx, Stream *stream );
static bool updateFrame( DecCtx *decCtx, Surface *surface );
static void testDecode( AppCtx *appCtx, int decodeIndex, int numFramesToDecode, Surface *surface, Async *async, Stream *stream );
static bool runUntilDone( AppCtx *appCtx );
static void discoverVideoDecoder( void );
static void showUsage( void );

void iprintf( int level, const char *fmt, ... )
{
   va_list argptr;

   if ( level <= gLogLevel )
   {
      va_start( argptr, fmt );
      if ( gReport )
      {
         vfprintf( gReport, fmt, argptr );
      }
      vprintf( fmt, argptr );
      va_end( argptr );
   }
}

static long long getCurrentTimeMillis(void)
{
   struct timeval tv;
   long long utcCurrentTimeMillis;

   gettimeofday(&tv,0);
   utcCurrentTimeMillis= tv.tv_sec*1000LL+(tv.tv_usec/1000LL);

   return utcCurrentTimeMillis;
}

static double getCpuIdle()
{
   double idle= 0.0;
   FILE *pFile= 0;
   char line[1024];
   int value[10];
   int numValue;
   int i, total;
   char *s;
   pFile= fopen("/proc/stat", "rt");
   if ( pFile )
   {
      s= fgets( line, sizeof(line), pFile );
      if ( s )
      {
         numValue= sscanf( s, "cpu %d %d %d %d %d %d %d %d %d %d",
                           &value[0], &value[1], &value[2], &value[3], &value[4],
                           &value[5], &value[6], &value[7], &value[8], &value[9] );
         if ( numValue == 10 )
         {
            total= 0;
            for( i= 0; i < numValue; ++i )
            {
               total += value[i];
            }
            idle= 100.0 * ((double)value[3] / (double)total);
         }
      }
      fclose( pFile );
   }
   return idle;
}

static void emitLoadAverage()
{
   double loadAverage= 0.0;
   FILE *pFile= 0;
   char line[1024];
   char *s;
   pFile= fopen("/proc/loadavg", "rt");
   if ( pFile )
   {
      s= fgets( line, sizeof(line), pFile );
      if ( s )
      {
         iprintf(0,"Load average: %s", s);
      }
      fclose( pFile );
   }
}

#define MAX_ATTRIBS (24)
#define RED_SIZE (8)
#define GREEN_SIZE (8)
#define BLUE_SIZE (8)
#define ALPHA_SIZE (8)
#define DEPTH_SIZE (0)

static bool initEGL( EGLCtx *eglCtx )
{
   bool result= false;
   AppCtx *ctx= eglCtx->appCtx;
   int i;
   EGLBoolean br;
   EGLint configCount;
   EGLConfig *configs= 0;
   EGLint attrs[MAX_ATTRIBS];
   EGLint redSize, greenSize, blueSize;
   EGLint alphaSize, depthSize;

   eglCtx->eglDisplay= EGL_NO_DISPLAY;
   eglCtx->eglContext= EGL_NO_CONTEXT;
   eglCtx->eglSurface= EGL_NO_SURFACE;

   if ( eglCtx->useWayland )
   {
      eglCtx->eglDisplay= PlatformGetEGLDisplayWayland( eglCtx->appCtx->platformCtx, eglCtx->dispWayland );
   }
   else
   {
      eglCtx->eglDisplay= PlatformGetEGLDisplay( eglCtx->appCtx->platformCtx, (NativeDisplayType)eglCtx->nativeDisplay );
   }
   if ( eglCtx->eglDisplay == EGL_NO_DISPLAY )
   {
      iprintf(0,"Error: initEGL: eglGetDisplay failed: %X\n", eglGetError() );
      goto exit;
   }

   br= eglInitialize( eglCtx->eglDisplay, &eglCtx->majorVersion, &eglCtx->minorVersion );
   if ( !br )
   {
      iprintf(0,"Error: initEGL: unable to initialize EGL display: %X\n", eglGetError() );
      goto exit;
   }

   br= eglGetConfigs( eglCtx->eglDisplay, NULL, 0, &configCount );
   if ( !br )
   {
      iprintf(0,"Error: initEGL: unable to get count of EGL configurations: %X\n", eglGetError() );
      goto exit;
   }

   configs= (EGLConfig*)malloc( configCount*sizeof(EGLConfig) );
   if ( !configs )
   {
      iprintf(0,"Error: initEGL: no memory for EGL configurations\n");
      goto exit;
   }

   i= 0;
   attrs[i++]= EGL_RED_SIZE;
   attrs[i++]= RED_SIZE;
   attrs[i++]= EGL_GREEN_SIZE;
   attrs[i++]= GREEN_SIZE;
   attrs[i++]= EGL_BLUE_SIZE;
   attrs[i++]= BLUE_SIZE;
   attrs[i++]= EGL_DEPTH_SIZE;
   attrs[i++]= DEPTH_SIZE;
   attrs[i++]= EGL_STENCIL_SIZE;
   attrs[i++]= 0;
   attrs[i++]= EGL_SURFACE_TYPE;
   attrs[i++]= EGL_WINDOW_BIT;
   attrs[i++]= EGL_RENDERABLE_TYPE;
   attrs[i++]= EGL_OPENGL_ES2_BIT;
   attrs[i++]= EGL_NONE;

   br= eglChooseConfig( eglCtx->eglDisplay, attrs, configs, configCount, &configCount );
   if ( !br )
   {
      iprintf(0,"Error: initEGL: eglChooseConfig failed: %X\n", eglGetError() );
      goto exit;
   }

   for( i= 0; i < configCount; ++i )
   {
      eglGetConfigAttrib( eglCtx->eglDisplay, configs[i], EGL_RED_SIZE, &redSize );
      eglGetConfigAttrib( eglCtx->eglDisplay, configs[i], EGL_GREEN_SIZE, &greenSize );
      eglGetConfigAttrib( eglCtx->eglDisplay, configs[i], EGL_BLUE_SIZE, &blueSize );
      eglGetConfigAttrib( eglCtx->eglDisplay, configs[i], EGL_ALPHA_SIZE, &alphaSize );
      eglGetConfigAttrib( eglCtx->eglDisplay, configs[i], EGL_DEPTH_SIZE, &depthSize );

      iprintf(0,"config %d: red: %d green: %d blue: %d alpha: %d depth: %d\n",
              i, redSize, greenSize, blueSize, alphaSize, depthSize );

      if ( (redSize == RED_SIZE) &&
           (greenSize == GREEN_SIZE) &&
           (blueSize == BLUE_SIZE) &&
           (alphaSize == ALPHA_SIZE) &&
           (depthSize >= DEPTH_SIZE) )
      {
         iprintf(0, "choosing config %d\n", i);
         break;
      }
   }
   if ( configCount == i )
   {
      iprintf(0,"Error: initEGL: no suitable configuration is available\n");
      goto exit;
   }
   eglCtx->eglConfig= configs[i];

   attrs[0]= EGL_CONTEXT_CLIENT_VERSION;
   attrs[1]= 2;
   attrs[2]= EGL_NONE;
    
   eglCtx->eglContext= eglCreateContext( eglCtx->eglDisplay, eglCtx->eglConfig, EGL_NO_CONTEXT, attrs );
   if ( eglCtx->eglContext == EGL_NO_CONTEXT )
   {
      iprintf(0, "Error: initEGL: eglCreateContext failed: %X\n", eglGetError() );
      goto exit;
   }

   eglCtx->nativeWindow= PlatformCreateNativeWindow( ctx->platformCtx, ctx->windowWidth, ctx->windowHeight );
   if ( !eglCtx->nativeWindow )
   {
      iprintf(0, "Error: initEGL: PlatformCreateNativeWindow failed\n");
      goto exit;
   }

   eglCtx->eglSurface= eglCreateWindowSurface( eglCtx->eglDisplay,
                                               eglCtx->eglConfig,
                                               (EGLNativeWindowType)eglCtx->nativeWindow,
                                               NULL );
   if ( eglCtx->eglSurface == EGL_NO_SURFACE )
   {
      iprintf(0, "Error: initEGL: eglCreateWindowSurface failed: %X\n", eglGetError());
      goto exit;
   }

   eglMakeCurrent( eglCtx->eglDisplay, eglCtx->eglSurface, eglCtx->eglSurface, eglCtx->eglContext );

   eglSwapInterval( eglCtx->eglDisplay, 1 );

   eglCtx->initialized= true;

   result= true;

exit:

   if ( configs )
   {
      free( configs );
   }

   return result;
}

static void termEGL( EGLCtx *eglCtx )
{
   AppCtx *ctx= eglCtx->appCtx;

   if ( eglCtx->eglDisplay != EGL_NO_DISPLAY )
   {
      eglMakeCurrent( eglCtx->eglDisplay, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT );
   }

   if ( eglCtx->eglSurface != EGL_NO_SURFACE )
   {
      eglDestroySurface( eglCtx->eglDisplay, eglCtx->eglSurface );
      eglCtx->eglSurface= EGL_NO_SURFACE;
   }

   if ( eglCtx->nativeWindow )
   {
      PlatformDestroyNativeWindow( ctx->platformCtx, eglCtx->nativeWindow );
   }

   if ( eglCtx->eglContext != EGL_NO_CONTEXT )
   {
      eglDestroyContext( eglCtx->eglDisplay, eglCtx->eglContext );
      eglCtx->eglContext= EGL_NO_CONTEXT;
   }

   if ( eglCtx->initialized )
   {      
      eglTerminate( eglCtx->eglDisplay );
      eglReleaseThread();
      eglCtx->initialized= false;
   }
}

static const char *vertColor=
   "uniform mat4 u_matrix;\n"
   "uniform vec4 u_offset;\n"
   "attribute vec4 pos;\n"
   "attribute vec4 color;\n"
   "varying vec4 v_color;\n"
   "void main()\n"
   "{\n"
   "  gl_Position= u_matrix * pos + u_offset;\n"
   "  v_color= color;\n"
   "}\n";

static const char *fragColor=
  "#ifdef GL_ES\n"
  "precision mediump float;\n"
  "#endif\n"
  "varying vec4 v_color;\n"
  "void main()\n"
  "{\n"
  "  gl_FragColor= v_color;\n"
  "}\n";

static const char *vertTexture=
  "attribute vec2 pos;\n"
  "attribute vec2 texcoord;\n"
  "uniform mat4 u_matrix;\n"
  "uniform vec2 u_resolution;\n"
  "varying vec2 tx;\n"
  "void main()\n"
  "{\n"
  "  vec4 v1= u_matrix * vec4(pos, 0, 1);\n"
  "  vec4 v2= v1 / vec4(u_resolution, u_resolution.x, 1);\n"
  "  vec4 v3= v2 * vec4(2.0, 2.0, 1, 1);\n"
  "  vec4 v4= v3 - vec4(1.0, 1.0, 0, 0);\n"
  "  v4.w= 1.0+v4.z;\n"
  "  gl_Position=  v4 * vec4(1, -1, 1, 1);\n"
  "  tx= texcoord;\n"
  "}\n";

static const char *fragTexture=
  "#extension GL_OES_EGL_image_external : require\n"
  "#ifdef GL_ES\n"
  "precision mediump float;\n"
  "#endif\n"
  "uniform samplerExternalOES texture;\n"
  "varying vec2 tx;\n"
  "void main()\n"
  "{\n"
  "  gl_FragColor= texture2D(texture, tx);\n"
  "}\n";

static const char *vertTextureYUV=
  "attribute vec2 pos;\n"
  "attribute vec2 texcoord;\n"
  "attribute vec2 texcoorduv;\n"
  "uniform mat4 u_matrix;\n"
  "uniform vec2 u_resolution;\n"
  "varying vec2 tx;\n"
  "varying vec2 txuv;\n"
  "void main()\n"
  "{\n"
  "  vec4 v1= u_matrix * vec4(pos, 0, 1);\n"
  "  vec4 v2= v1 / vec4(u_resolution, u_resolution.x, 1);\n"
  "  vec4 v3= v2 * vec4(2.0, 2.0, 1, 1);\n"
  "  vec4 v4= v3 - vec4(1.0, 1.0, 0, 0);\n"
  "  v4.w= 1.0+v4.z;\n"
  "  gl_Position=  v4 * vec4(1, -1, 1, 1);\n"
  "  tx= texcoord;\n"
  "  txuv= texcoorduv;\n"
  "}\n";

static const char *fragTextureYUV=
  "#ifdef GL_ES\n"
  "precision mediump float;\n"
  "#endif\n"
  "uniform sampler2D texture;\n"
  "uniform sampler2D textureuv;\n"
  "const vec3 cc_r= vec3(1.0, -0.8604, 1.59580);\n"
  "const vec4 cc_g= vec4(1.0, 0.539815, -0.39173, -0.81290);\n"
  "const vec3 cc_b= vec3(1.0, -1.071, 2.01700);\n"
  "varying vec2 tx;\n"
  "varying vec2 txuv;\n"
  "void main()\n"
  "{\n"
  "   vec4 y_vec= texture2D(texture, tx);\n"
  "   vec4 c_vec= texture2D(textureuv, txuv);\n"
  "   vec4 temp_vec= vec4(y_vec.r, 1.0, c_vec.r, c_vec.g);\n"
  "   gl_FragColor= vec4( dot(cc_r,temp_vec.xyw), dot(cc_g,temp_vec), dot(cc_b,temp_vec.xyz), 1 );\n"
  "}\n";

static bool initGL( GLCtx *ctx )
{
   bool result= false;
   GLint status;
   GLsizei length;
   char infoLog[512];
   const char *fragSrc, *vertSrc;

   ctx->eglCreateImageKHR= (PFNEGLCREATEIMAGEKHRPROC)eglGetProcAddress("eglCreateImageKHR");
   if ( !ctx->eglCreateImageKHR )
   {
      iprintf(0,"Error: initGL: no eglCreateImageKHR\n");
      goto exit;
   }

   ctx->eglDestroyImageKHR= (PFNEGLDESTROYIMAGEKHRPROC)eglGetProcAddress("eglDestroyImageKHR");
   if ( !ctx->eglDestroyImageKHR )
   {
      iprintf(0,"Error: initGL: no eglDestroyImageKHR\n");
      goto exit;
   }

   ctx->glEGLImageTargetTexture2DOES= (PFNGLEGLIMAGETARGETTEXTURE2DOESPROC)eglGetProcAddress("glEGLImageTargetTexture2DOES");
   if ( !ctx->glEGLImageTargetTexture2DOES )
   {
      iprintf(0,"Error: initGL: no glEGLImageTargetTexture2DOES\n");
      goto exit;
   }


   ctx->fragColor= glCreateShader( GL_FRAGMENT_SHADER );
   if ( !ctx->fragColor )
   {
      iprintf(0,"Error: initGL: failed to create color fragment shader\n");
      goto exit;
   }

   glShaderSource( ctx->fragColor, 1, (const char **)&fragColor, NULL );
   glCompileShader( ctx->fragColor );
   glGetShaderiv( ctx->fragColor, GL_COMPILE_STATUS, &status );
   if ( !status )
   {
      glGetShaderInfoLog( ctx->fragColor, sizeof(infoLog), &length, infoLog );
      iprintf(0,"Error: initGL: compiling color fragment shader: %*s\n", length, infoLog );
      goto exit;
   }

   ctx->vertColor= glCreateShader( GL_VERTEX_SHADER );
   if ( !ctx->vertColor )
   {
      iprintf(0,"Error: initGL: failed to create color vertex shader\n");
      goto exit;
   }

   glShaderSource( ctx->vertColor, 1, (const char **)&vertColor, NULL );
   glCompileShader( ctx->vertColor );
   glGetShaderiv( ctx->vertColor, GL_COMPILE_STATUS, &status );
   if ( !status )
   {
      glGetShaderInfoLog( ctx->vertColor, sizeof(infoLog), &length, infoLog );
      iprintf(0,"Error: initGL: compiling color vertex shader: \n%*s\n", length, infoLog );
      goto exit;
   }

   ctx->progColor= glCreateProgram();
   glAttachShader(ctx->progColor, ctx->fragColor);
   glAttachShader(ctx->progColor, ctx->vertColor);

   ctx->locPosColor= 0;
   ctx->locColorColor= 1;
   glBindAttribLocation(ctx->progColor, ctx->locPosColor, "pos");
   glBindAttribLocation(ctx->progColor, ctx->locColorColor, "color");

   glLinkProgram(ctx->progColor);
   glGetProgramiv(ctx->progColor, GL_LINK_STATUS, &status);
   if (!status)
   {
      glGetProgramInfoLog(ctx->progColor, sizeof(infoLog), &length, infoLog);
      iprintf(0,"Error: initGL: linking:\n%*s\n", length, infoLog);
      goto exit;
   }

   ctx->locOffsetColor= glGetUniformLocation(ctx->progColor, "u_offset");
   ctx->locMatrixColor= glGetUniformLocation(ctx->progColor, "u_matrix");



   if ( ctx->haveYUVShaders )
   {
      fragSrc= fragTextureYUV;
      vertSrc= vertTextureYUV;
   }
   else
   {
      fragSrc= fragTexture;
      vertSrc= vertTexture;
   }

   ctx->fragTex= glCreateShader( GL_FRAGMENT_SHADER );
   if ( !ctx->fragTex )
   {
      iprintf(0,"Error: initGL: failed to create texture fragment shader\n");
      goto exit;
   }

   glShaderSource( ctx->fragTex, 1, (const char **)&fragSrc, NULL );
   glCompileShader( ctx->fragTex );
   glGetShaderiv( ctx->fragTex, GL_COMPILE_STATUS, &status );
   if ( !status )
   {
      glGetShaderInfoLog( ctx->fragTex, sizeof(infoLog), &length, infoLog );
      iprintf(0,"Error: initGL: compiling texture fragment shader: %*s\n", length, infoLog );
      goto exit;
   }

   ctx->vertTex= glCreateShader( GL_VERTEX_SHADER );
   if ( !ctx->vertTex )
   {
      iprintf(0,"Error: initGL: failed to create texture vertex shader\n");
      goto exit;
   }

   glShaderSource( ctx->vertTex, 1, (const char **)&vertSrc, NULL );
   glCompileShader( ctx->vertTex );
   glGetShaderiv( ctx->vertTex, GL_COMPILE_STATUS, &status );
   if ( !status )
   {
      glGetShaderInfoLog( ctx->vertTex, sizeof(infoLog), &length, infoLog );
      iprintf(0,"Error: initGL: compiling texture vertex shader: \n%*s\n", length, infoLog );
      goto exit;
   }

   ctx->progTex= glCreateProgram();
   glAttachShader(ctx->progTex, ctx->fragTex);
   glAttachShader(ctx->progTex, ctx->vertTex);

   ctx->locPosTex= 0;
   ctx->locTC= 1;
   glBindAttribLocation(ctx->progTex, ctx->locPosTex, "pos");
   glBindAttribLocation(ctx->progTex, ctx->locTC, "texcoord");
   if ( ctx->haveYUVShaders )
   {
      ctx->locTCUV= 2;
      glBindAttribLocation(ctx->progTex, ctx->locTCUV, "texcoorduv");
   }

   glLinkProgram(ctx->progTex);
   glGetProgramiv(ctx->progTex, GL_LINK_STATUS, &status);
   if (!status)
   {
      glGetProgramInfoLog(ctx->progTex, sizeof(infoLog), &length, infoLog);
      iprintf(0,"Error: initGL: linking:\n%*s\n", length, infoLog);
      goto exit;
   }

   ctx->locResTex= glGetUniformLocation(ctx->progTex,"u_resolution");
   ctx->locMatrixTex= glGetUniformLocation(ctx->progTex,"u_matrix");
   ctx->locTexture= glGetUniformLocation(ctx->progTex,"texture");
   if ( ctx->haveYUVShaders )
   {
      ctx->locTextureUV= glGetUniformLocation(ctx->progTex,"textureuv");
   }

   result= true;

exit:

   return result;
}

static void termGL( GLCtx *glCtx )
{
   if ( glCtx->fragTex )
   {
      glDeleteShader( glCtx->fragTex );
      glCtx->fragTex= 0;
   }
   if ( glCtx->vertTex )
   {
      glDeleteShader( glCtx->vertTex );
      glCtx->vertTex= 0;
   }
   if ( glCtx->progTex )
   {
      glDeleteProgram( glCtx->progTex );
      glCtx->progTex= 0;
   }
}

static void drawSurface( GLCtx *glCtx, Surface *surface )
{
   AppCtx *appCtx= glCtx->appCtx;
   int x, y, w, h;
   GLenum glerr;

   x= surface->x;
   y= surface->y;
   w= surface->w;
   h= surface->h;
 
   const float verts[4][2]=
   {
      { float(x), float(y) },
      { float(x+w), float(y) },
      { float(x),  float(y+h) },
      { float(x+w), float(y+h) }
   };
 
   const float uv[4][2]=
   {
      { 0,  0 },
      { 1,  0 },
      { 0,  1 },
      { 1,  1 }
   };

   const float identityMatrix[4][4]=
   {
      {1, 0, 0, 0},
      {0, 1, 0, 0},
      {0, 0, 1, 0},
      {0, 0, 0, 1}
   };

   if ( glCtx->haveYUVShaders != surface->haveYUVTextures )
   {
      termGL( glCtx );
      glCtx->haveYUVShaders= surface->haveYUVTextures;
      if ( !initGL( glCtx ) )
      {
         iprintf(0,"Error: drawSurface: initGL failed while changing shaders\n");
      }
   }

   if ( (surface->textureId[0] == GL_NONE) || surface->externalImage )
   {
      for( int i= 0; i < surface->textureCount; ++i )
      {
         if ( surface->textureId[i] == GL_NONE )
         {
            glGenTextures(1, &surface->textureId[i] );
         }
       
         glActiveTexture(GL_TEXTURE0+i);
         glBindTexture(GL_TEXTURE_2D, surface->textureId[i] );
         if ( surface->eglImage[i] )
         {
            if ( surface->externalImage )
            {
               glCtx->glEGLImageTargetTexture2DOES(GL_TEXTURE_EXTERNAL_OES, surface->eglImage[i]);
            }
            else
            {
               glCtx->glEGLImageTargetTexture2DOES(GL_TEXTURE_2D, surface->eglImage[i]);
            }
         }
         glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
         glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
         glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
         glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
      }
   }

   glUseProgram(glCtx->progTex);
   glUniform2f(glCtx->locResTex, appCtx->windowWidth, appCtx->windowHeight);
   glUniformMatrix4fv(glCtx->locMatrixTex, 1, GL_FALSE, (GLfloat*)identityMatrix);

   glActiveTexture(GL_TEXTURE0); 
   glBindTexture(GL_TEXTURE_2D, surface->textureId[0]);
   glUniform1i(glCtx->locTexture, 0);
   glVertexAttribPointer(glCtx->locPosTex, 2, GL_FLOAT, GL_FALSE, 0, verts);
   glVertexAttribPointer(glCtx->locTC, 2, GL_FLOAT, GL_FALSE, 0, uv);
   glEnableVertexAttribArray(glCtx->locPosTex);
   glEnableVertexAttribArray(glCtx->locTC);
   if ( surface->haveYUVTextures )
   {
      glActiveTexture(GL_TEXTURE1); 
      glBindTexture(GL_TEXTURE_2D, surface->textureId[1]);
      glUniform1i(glCtx->locTextureUV, 1);
      glVertexAttribPointer(glCtx->locTCUV, 2, GL_FLOAT, GL_FALSE, 0, uv);
      glEnableVertexAttribArray(glCtx->locTCUV);
   }
   glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
   glDisableVertexAttribArray(glCtx->locPosTex);
   glDisableVertexAttribArray(glCtx->locTC);
   if ( surface->haveYUVTextures )
   {
      glDisableVertexAttribArray(glCtx->locTCUV);
   }

   glerr= glGetError();
   if ( glerr != GL_NO_ERROR )
   {
      iprintf(0,"Warning: drawSurface: glGetError: %X\n", glerr);
   }
}

static int ioctl_wrapper( int fd, int request, void* arg )
{
   const char *req= 0;
   int rc;

   if ( gVerbose )
   {
      switch( request )
      {
         case VIDIOC_QUERYCAP: req= "VIDIOC_QUERYCAP"; break;
         case VIDIOC_ENUM_FMT: req= "VIDIOC_ENUM_FMT"; break;
         case VIDIOC_G_FMT: req= "VIDIOC_G_FMT"; break;
         case VIDIOC_S_FMT: req= "VIDIOC_S_FMT"; break;
         case VIDIOC_REQBUFS: req= "VIDIOC_REQBUFS"; break;
         case VIDIOC_QUERYBUF: req= "VIDIOC_QUERYBUFS"; break;
         case VIDIOC_G_FBUF: req= "VIDIOC_G_FBUF"; break;
         case VIDIOC_S_FBUF: req= "VIDIOC_S_FBUF"; break;
         case VIDIOC_OVERLAY: req= "VIDIOC_OVERLAY"; break;
         case VIDIOC_QBUF: req= "VIDIOC_QBUF"; break;
         case VIDIOC_EXPBUF: req= "VIDIOC_EXPBUF"; break;
         case VIDIOC_DQBUF: req= "VIDIOC_DQBUF"; break;
         case VIDIOC_STREAMON: req= "VIDIOC_STREAMON"; break;
         case VIDIOC_STREAMOFF: req= "VIDIOC_STREAMOFF"; break;
         case VIDIOC_G_PARM: req= "VIDIOC_G_PARM"; break;
         case VIDIOC_S_PARM: req= "VIDIOC_S_PARM"; break;
         case VIDIOC_G_STD: req= "VIDIOC_G_STD"; break;
         case VIDIOC_S_STD: req= "VIDIOC_S_STD"; break;
         case VIDIOC_ENUMSTD: req= "VIDIOC_ENUMSTD"; break;
         case VIDIOC_ENUMINPUT: req= "VIDIOC_ENUMINPUT"; break;
         case VIDIOC_G_CTRL: req= "VIDIOC_G_CTRL"; break;
         case VIDIOC_S_CTRL: req= "VIDIOC_S_CTRL"; break;
         case VIDIOC_QUERYCTRL: req= "VIDIOC_QUERYCTRL"; break;
         case VIDIOC_ENUM_FRAMESIZES: req= "VIDIOC_ENUM_FRAMESIZES"; break;
         case VIDIOC_TRY_FMT: req= "VIDIOC_TRY_FMT"; break;
         case VIDIOC_CROPCAP: req= "VIDIOC_CROPCAP"; break;
         case VIDIOC_CREATE_BUFS: req= "VIDIOC_CREATE_BUFS"; break;
         case VIDIOC_G_SELECTION: req= "VIDIOC_G_SELECTION"; break;
         default: req= "NA"; break;
      }
      printf("ioct( %d, %x ( %s ) )\n", fd, request, req );
      if ( request == VIDIOC_S_FMT )
      {
         struct v4l2_format *format= (struct v4l2_format*)arg;
         printf(": type %d\n", format->type);
         if ( (format->type == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE) ||
              (format->type == V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE) )
         {
            printf("pix_mp: pixelFormat %X w %d h %d field %d cs %d flg %x num_planes %d p0: sz %d bpl %d p1: sz %d bpl %d\n",
                    format->fmt.pix_mp.pixelformat,
                    format->fmt.pix_mp.width,
                    format->fmt.pix_mp.height,
                    format->fmt.pix_mp.field,
                    format->fmt.pix_mp.colorspace,
                    format->fmt.pix_mp.flags,
                    format->fmt.pix_mp.num_planes,
                    format->fmt.pix_mp.plane_fmt[0].sizeimage,
                    format->fmt.pix_mp.plane_fmt[0].bytesperline,
                    format->fmt.pix_mp.plane_fmt[1].sizeimage,
                    format->fmt.pix_mp.plane_fmt[1].bytesperline
                  );
         }
      }
      else if ( request == VIDIOC_REQBUFS )
      {
         struct v4l2_requestbuffers *rb= (struct v4l2_requestbuffers*)arg;
         printf("count %d type %d mem %d\n", rb->count, rb->type, rb->memory);
      }
      else if ( request == VIDIOC_CREATE_BUFS )
      {
         struct v4l2_create_buffers *cb= (struct v4l2_create_buffers*)arg;
         struct v4l2_format *format= &cb->format;
         printf("count %d mem %d\n", cb->count, cb->memory);
         printf("pix_mp: pixelFormat %X w %d h %d num_planes %d p0: sz %d bpl %d p1: sz %d bpl %d\n",
                 format->fmt.pix_mp.pixelformat,
                 format->fmt.pix_mp.width,
                 format->fmt.pix_mp.height,
                 format->fmt.pix_mp.num_planes,
                 format->fmt.pix_mp.plane_fmt[0].sizeimage,
                 format->fmt.pix_mp.plane_fmt[0].bytesperline,
                 format->fmt.pix_mp.plane_fmt[1].sizeimage,
                 format->fmt.pix_mp.plane_fmt[1].bytesperline
               );
      }
      else if ( request == VIDIOC_QBUF )
      {
         struct v4l2_buffer *buf= (struct v4l2_buffer*)arg;
         printf("buff: index %d q: type %d bytesused %d flags %X field %d mem %x length %d timestamp sec %ld usec %ld\n",
                buf->index, buf->type, buf->bytesused, buf->flags, buf->field, buf->memory, buf->length, buf->timestamp.tv_sec, buf->timestamp.tv_usec);
         if ( buf->m.planes &&
              ( (buf->type == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE) ||
                (buf->type == V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE) ) )
         {
            printf("buff: p0: bu %d len %d moff %d doff %d p1: bu %d len %d moff %d doff %d\n",
                   buf->m.planes[0].bytesused, buf->m.planes[0].length, buf->m.planes[0].m.mem_offset, buf->m.planes[0].data_offset,
                   buf->m.planes[1].bytesused, buf->m.planes[1].length, buf->m.planes[1].m.mem_offset, buf->m.planes[1].data_offset );
         }
      }
      else if ( request == VIDIOC_DQBUF )
      {
         struct v4l2_buffer *buf= (struct v4l2_buffer*)arg;
         printf("buff: index %d s dq: type %d bytesused %d flags %X field %d mem %x length %d timestamp sec %ld usec %ld\n",
                buf->index, buf->type, buf->bytesused, buf->flags, buf->field, buf->memory, buf->length, buf->timestamp.tv_sec, buf->timestamp.tv_usec);
         if ( buf->m.planes &&
              ( (buf->type == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE) ||
                (buf->type == V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE) ) )
         {
            printf("buff: p0: bu %d len %d moff %d doff %d p1: bu %d len %d moff %d doff %d\n",
                   buf->m.planes[0].bytesused, buf->m.planes[0].length, buf->m.planes[0].m.mem_offset, buf->m.planes[0].data_offset,
                   buf->m.planes[1].bytesused, buf->m.planes[1].length, buf->m.planes[1].m.mem_offset, buf->m.planes[1].data_offset );
         }
      }
      else if ( (request == VIDIOC_STREAMON) || (request == VIDIOC_STREAMOFF) )
      {
         int *type= (int*)arg;
         printf(": type %d\n", *type);
      }
   }

   rc= ioctl( fd, request, arg );


   if ( gVerbose )
   {
      if ( rc < 0 )
      {
         printf("ioct( %d, %x ) rc %d errno %d\n", fd, request, rc, errno );
      }
      else
      {
         printf("ioct( %d, %x ) rc %d\n", fd, request, rc );
         if ( request == VIDIOC_S_FMT )
         {
            struct v4l2_format *format= (struct v4l2_format*)arg;
            if ( (format->type == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE) ||
                 (format->type == V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE) )
            {
               printf("pix_mp: pixelFormat %X w %d h %d num_planes %d p0: sz %d bpl %d p1: sz %d bpl %d\n",
                       format->fmt.pix_mp.pixelformat,
                       format->fmt.pix_mp.width,
                       format->fmt.pix_mp.height,
                       format->fmt.pix_mp.num_planes,
                       format->fmt.pix_mp.plane_fmt[0].sizeimage,
                       format->fmt.pix_mp.plane_fmt[0].bytesperline,
                       format->fmt.pix_mp.plane_fmt[1].sizeimage,
                       format->fmt.pix_mp.plane_fmt[1].bytesperline
                     );
            }
         }
         else if ( request == VIDIOC_CREATE_BUFS )
         {
            struct v4l2_create_buffers *cb= (struct v4l2_create_buffers*)arg;
            printf("index %d count %d mem %d\n", cb->index, cb->count, cb->memory);
         }
         else if ( request == VIDIOC_G_CTRL )
         {
            struct v4l2_control *ctrl= (struct v4l2_control*)arg;
            printf("id %d value %d\n", ctrl->id, ctrl->value);
         }
         else if ( request == VIDIOC_DQBUF )
         {
            struct v4l2_buffer *buf= (struct v4l2_buffer*)arg;
            printf("buff: index %d f dq: type %d bytesused %d flags %X field %d mem %x length %d seq %d timestamp sec %ld usec %ld\n",
                   buf->index, buf->type, buf->bytesused, buf->flags, buf->field, buf->memory, buf->length, buf->sequence, buf->timestamp.tv_sec, buf->timestamp.tv_usec);
            if ( buf->m.planes &&
                 ( (buf->type == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE) ||
                   (buf->type == V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE) ) )
            {
               printf("buff: p0: bu %d len %d moff %d doff %d p1: bu %d len %d moff %d doff %d\n",
                      buf->m.planes[0].bytesused, buf->m.planes[0].length, buf->m.planes[0].m.mem_offset, buf->m.planes[0].data_offset,
                      buf->m.planes[1].bytesused, buf->m.planes[1].length, buf->m.planes[1].m.mem_offset, buf->m.planes[1].data_offset );
            }
         }
      }
   }

   return rc;
}

static bool getInputFormats( V4l2Ctx *v4l2 )
{
   bool result= false;
   struct v4l2_fmtdesc format;
   int i, rc;
   int32_t bufferType;

   bufferType= (v4l2->isMultiPlane ? V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE : V4L2_BUF_TYPE_VIDEO_OUTPUT);

   i= 0;
   for( ; ; )
   {
      format.index= i;
      format.type= bufferType;
      rc= IOCTL( v4l2->v4l2Fd, VIDIOC_ENUM_FMT, &format);
      if ( rc < 0 )
      {
         if ( errno == EINVAL )
         {
            iprintf(1,"Found %d input formats\n", i);
            v4l2->numInputFormats= i;
            break;
         }
         goto exit;
      }
      ++i;
   }

   v4l2->inputFormats= (struct v4l2_fmtdesc*)calloc( v4l2->numInputFormats, sizeof(struct v4l2_format) );
   if ( !v4l2->inputFormats )
   {
      iprintf(0,"Error: getInputFormats: no memory for inputFormats\n");
      v4l2->numInputFormats= 0;
      goto exit;
   }

   for( i= 0; i < v4l2->numInputFormats; ++i)
   {
      v4l2->inputFormats[i].index= i;
      v4l2->inputFormats[i].type= bufferType;
      rc= IOCTL( v4l2->v4l2Fd, VIDIOC_ENUM_FMT, &v4l2->inputFormats[i]);
      if ( rc < 0 )
      {
         goto exit;
      }
      iprintf(1, "input format %d: flags %08x pixelFormat: %x desc: %s\n", 
             i, v4l2->inputFormats[i].flags, v4l2->inputFormats[i].pixelformat, v4l2->inputFormats[i].description );
   }

   result= true;

exit:
   return result;
}

static bool getOutputFormats( V4l2Ctx *v4l2 )
{
   bool result= false;
   struct v4l2_fmtdesc format;
   int i, rc;
   int32_t bufferType;

   bufferType= (v4l2->isMultiPlane ? V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE : V4L2_BUF_TYPE_VIDEO_CAPTURE);

   i= 0;
   for( ; ; )
   {
      format.index= i;
      format.type= bufferType;
      rc= IOCTL( v4l2->v4l2Fd, VIDIOC_ENUM_FMT, &format);
      if ( rc < 0 )
      {
         if ( errno == EINVAL )
         {
            iprintf(1,"Found %d output formats\n", i);
            v4l2->numOutputFormats= i;
            break;
         }
         goto exit;
      }
      ++i;
   }

   v4l2->outputFormats= (struct v4l2_fmtdesc*)calloc( v4l2->numOutputFormats, sizeof(struct v4l2_format) );
   if ( !v4l2->outputFormats )
   {
      iprintf(0,"Error: getOutputFormats: no memory for outputFormats\n");
      v4l2->numOutputFormats= 0;
      goto exit;
   }

   for( i= 0; i < v4l2->numOutputFormats; ++i)
   {
      v4l2->outputFormats[i].index= i;
      v4l2->outputFormats[i].type= bufferType;
      rc= IOCTL( v4l2->v4l2Fd, VIDIOC_ENUM_FMT, &v4l2->outputFormats[i]);
      if ( rc < 0 )
      {
         goto exit;
      }
      iprintf(1,"output format %d: flags %08x pixelFormat: %x desc: %s\n", 
             i, v4l2->outputFormats[i].flags, v4l2->outputFormats[i].pixelformat, v4l2->outputFormats[i].description );
   }

   result= true;

exit:
   return result;
}

static bool setInputFormat( V4l2Ctx *v4l2 )
{
   bool result= false;
   int rc;
   int32_t bufferType;

   bufferType= (v4l2->isMultiPlane ? V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE : V4L2_BUF_TYPE_VIDEO_OUTPUT);

   memset( &v4l2->fmtIn, 0, sizeof(struct v4l2_format) );
   v4l2->fmtIn.type= bufferType;
   if ( v4l2->isMultiPlane )
   {
      v4l2->fmtIn.fmt.pix_mp.pixelformat= v4l2->inputFormat;
      v4l2->fmtIn.fmt.pix_mp.width= v4l2->decCtx->videoWidth;
      v4l2->fmtIn.fmt.pix_mp.height= v4l2->decCtx->videoHeight;
      v4l2->fmtIn.fmt.pix_mp.num_planes= 1;
      v4l2->fmtIn.fmt.pix_mp.plane_fmt[0].sizeimage= 1024*1024;
      v4l2->fmtIn.fmt.pix_mp.plane_fmt[0].bytesperline= 0;
      v4l2->fmtIn.fmt.pix_mp.field= V4L2_FIELD_NONE;
   }
   else
   {
      v4l2->fmtIn.fmt.pix.pixelformat= v4l2->inputFormat;
      v4l2->fmtIn.fmt.pix.width= v4l2->decCtx->videoWidth;
      v4l2->fmtIn.fmt.pix.height= v4l2->decCtx->videoHeight;
      v4l2->fmtIn.fmt.pix.sizeimage= 1024*1024;
      v4l2->fmtIn.fmt.pix.field= V4L2_FIELD_NONE;
   }
   rc= IOCTL( v4l2->v4l2Fd, VIDIOC_S_FMT, &v4l2->fmtIn );
   if ( rc < 0 )
   {
      iprintf(0,"setInputFormat: decoder %d failed to set format for input: rc %d errno %d\n", v4l2->decCtx->decodeIndex, rc, errno);
      goto exit;
   }

   result= true;

exit:
   return result;
}


static bool setOutputFormat( V4l2Ctx *v4l2 )
{
   bool result= false;
   int rc;
   int32_t bufferType;

   bufferType= (v4l2->isMultiPlane ? V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE : V4L2_BUF_TYPE_VIDEO_CAPTURE);

   memset( &v4l2->fmtOut, 0, sizeof(struct v4l2_format) );
   v4l2->fmtOut.type= bufferType;

   /* Get current settings from driver */
   rc= IOCTL( v4l2->v4l2Fd, VIDIOC_G_FMT, &v4l2->fmtOut );
   if ( rc < 0 )
   {
      iprintf(0,"setOutputFormat: failed get format for output: rc %d errno %d\n", rc, errno);
   }

   if ( v4l2->isMultiPlane )
   {
      v4l2->fmtOut.fmt.pix_mp.pixelformat= V4L2_PIX_FMT_NV12;
      v4l2->fmtOut.fmt.pix_mp.width= v4l2->decCtx->videoWidth;
      v4l2->fmtOut.fmt.pix_mp.height= v4l2->decCtx->videoHeight;
      v4l2->fmtOut.fmt.pix_mp.num_planes= 2;
      v4l2->fmtOut.fmt.pix_mp.plane_fmt[0].sizeimage= v4l2->decCtx->videoWidth*v4l2->decCtx->videoHeight;
      v4l2->fmtOut.fmt.pix_mp.plane_fmt[0].bytesperline= v4l2->decCtx->videoWidth;
      v4l2->fmtOut.fmt.pix_mp.plane_fmt[1].sizeimage= v4l2->decCtx->videoWidth*v4l2->decCtx->videoHeight/2;
      v4l2->fmtOut.fmt.pix_mp.plane_fmt[1].bytesperline= v4l2->decCtx->videoWidth;
      v4l2->fmtOut.fmt.pix_mp.field= V4L2_FIELD_NONE;
   }
   else
   {
      v4l2->fmtOut.fmt.pix.pixelformat= V4L2_PIX_FMT_NV12;
      v4l2->fmtOut.fmt.pix.width= v4l2->decCtx->videoWidth;
      v4l2->fmtOut.fmt.pix.height= v4l2->decCtx->videoHeight;
      v4l2->fmtOut.fmt.pix.sizeimage= (v4l2->fmtOut.fmt.pix.width*v4l2->fmtOut.fmt.pix.height*3)/2;
      v4l2->fmtOut.fmt.pix.field= V4L2_FIELD_NONE;
   }
   rc= IOCTL( v4l2->v4l2Fd, VIDIOC_S_FMT, &v4l2->fmtOut );
   if ( rc < 0 )
   {
      iprintf(0,"setOutputFormat: decoder %d failed to set format for output: rc %d errno %d\n", v4l2->decCtx->decodeIndex, rc, errno);
      goto exit;
   }

   result= true;

exit:
   return result;
}

static bool setupInputBuffers( V4l2Ctx *v4l2 )
{
   bool result= false;
   int rc, neededBuffers;
   struct v4l2_control ctl;
   struct v4l2_requestbuffers reqbuf;
   struct v4l2_buffer *bufIn;
   void *bufStart;
   int32_t bufferType;
   uint32_t memOffset, memLength, memBytesUsed;

   bufferType= (v4l2->isMultiPlane ? V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE : V4L2_BUF_TYPE_VIDEO_OUTPUT);

   neededBuffers= NUM_INPUT_BUFFERS;

   memset( &ctl, 0, sizeof(ctl));
   ctl.id= V4L2_CID_MIN_BUFFERS_FOR_OUTPUT;
   rc= IOCTL( v4l2->v4l2Fd, VIDIOC_G_CTRL, &ctl );
   if ( rc == 0 )
   {
      v4l2->minBuffersIn= ctl.value;
      if ( v4l2->minBuffersIn != 0 )
      {
         neededBuffers= v4l2->minBuffersIn;
      }
   }

   if ( v4l2->minBuffersIn == 0 )
   {
      v4l2->minBuffersIn= MIN_INPUT_BUFFERS;
   }

   memset( &reqbuf, 0, sizeof(reqbuf) );
   reqbuf.count= neededBuffers;
   reqbuf.type= bufferType;
   reqbuf.memory= V4L2_MEMORY_MMAP;
   rc= IOCTL( v4l2->v4l2Fd, VIDIOC_REQBUFS, &reqbuf );
   if ( rc < 0 )
   {
      iprintf(0,"Error: setupInputBuffers: decoder %d failed to request %d mmap buffers for input: rc %d errno %d\n", v4l2->decCtx->decodeIndex, neededBuffers, rc, errno);
      goto exit;
   }
   v4l2->numBuffersIn= reqbuf.count;

   if ( reqbuf.count < v4l2->minBuffersIn )
   {
      iprintf(0,"Error: setupInputBuffers: decoder %d insufficient buffers: (%d versus %d)\n", v4l2->decCtx->decodeIndex, reqbuf.count, neededBuffers );
      goto exit;
   }

   v4l2->inBuffers= (BufferInfo*)calloc( reqbuf.count, sizeof(BufferInfo) );
   if ( !v4l2->inBuffers )
   {
      iprintf(0,"Error: setupInputBuffers: decoder %d no memory for BufferInfo\n", v4l2->decCtx->decodeIndex );
      goto exit;
   }

   for( int i= 0; i < reqbuf.count; ++i )
   {
      bufIn= &v4l2->inBuffers[i].buf;
      bufIn->type= bufferType;
      bufIn->index= i;
      bufIn->memory= V4L2_MEMORY_MMAP;
      if ( v4l2->isMultiPlane )
      {
         memset( v4l2->inBuffers[i].planes, 0, sizeof(struct v4l2_plane)*MAX_PLANES);
         bufIn->m.planes= v4l2->inBuffers[i].planes;
         bufIn->length= 3;
      }
      rc= IOCTL( v4l2->v4l2Fd, VIDIOC_QUERYBUF, bufIn );
      if ( rc < 0 )
      {
         iprintf(0,"Error: setupInputBuffers: decoder %d failed to query input buffer %d: rc %d errno %d\n", v4l2->decCtx->decodeIndex, i, rc, errno);
         goto exit;
      }
      if ( v4l2->isMultiPlane )
      {
         if ( bufIn->length != 1 )
         {
            iprintf(0,"setupInputBuffers: decoder %d num planes expected to be 1 for compressed input but is %d\n", v4l2->decCtx->decodeIndex, bufIn->length);
            goto exit;
         }
         v4l2->inBuffers[i].planeCount= 0;
         memOffset= bufIn->m.planes[0].m.mem_offset;
         memLength= bufIn->m.planes[0].length;
         memBytesUsed= bufIn->m.planes[0].bytesused;
      }
      else
      {
         memOffset= bufIn->m.offset;
         memLength= bufIn->length;
         memBytesUsed= bufIn->bytesused;
      }

      bufStart= mmap( NULL,
                      memLength,
                      PROT_READ | PROT_WRITE,
                      MAP_SHARED,
                      v4l2->v4l2Fd,
                      memOffset );
      if ( bufStart == MAP_FAILED )
      {
         iprintf(0,"Error: setupInputBuffers: decoder %d failed to mmap input buffer %d: errno %d\n", v4l2->decCtx->decodeIndex, i, errno);
         goto exit;
      }

      iprintf(2,"Input buffer: %d\n", i);
      iprintf(2,"  index: %d start: %p bytesUsed %d  offset %d length %d flags %08x\n", 
              bufIn->index, bufStart, memBytesUsed, memOffset, memLength, bufIn->flags );

      v4l2->inBuffers[i].fd= -1;
      v4l2->inBuffers[i].start= bufStart;
      v4l2->inBuffers[i].capacity= memLength;
   }

   result= true;

exit:

   if ( !result )
   {
      tearDownInputBuffers( v4l2 );
   }

   return result;
}

static void tearDownInputBuffers( V4l2Ctx *v4l2 )
{
   int rc;
   struct v4l2_requestbuffers reqbuf;
   int32_t bufferType;

   bufferType= (v4l2->isMultiPlane ? V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE : V4L2_BUF_TYPE_VIDEO_OUTPUT);

   if ( v4l2->inBuffers )
   {
      rc= IOCTL( v4l2->v4l2Fd, VIDIOC_STREAMOFF, &v4l2->fmtIn.type );
      if ( rc < 0 )
      {
         iprintf(0,"Error: tearDownInputBuffers: decder %d streamoff failed for input: rc %d errno %d\n", v4l2->decCtx->decodeIndex, rc, errno );
      }

      for( int i= 0; i < v4l2->numBuffersIn; ++i )
      {
         if ( v4l2->inBuffers[i].start )
         {
            munmap( v4l2->inBuffers[i].start, v4l2->inBuffers[i].capacity );
         }
      }
      free( v4l2->inBuffers );
      v4l2->inBuffers= 0;
   }

   if ( v4l2->numBuffersIn )
   {
      memset( &reqbuf, 0, sizeof(reqbuf) );
      reqbuf.count= 0;
      reqbuf.type= bufferType;
      reqbuf.memory= V4L2_MEMORY_MMAP;
      rc= IOCTL( v4l2->v4l2Fd, VIDIOC_REQBUFS, &reqbuf );
      if ( rc < 0 )
      {
         iprintf(0,"Error: tearDownInputBuffers: decoder %d failed to release v4l2 buffers for input: rc %d errno %d\n", v4l2->decCtx->decodeIndex, rc, errno);
      }
      v4l2->numBuffersIn= 0;
   }
}

static bool setupOutputBuffers( V4l2Ctx *v4l2 )
{
   bool result= false;
   int rc, neededBuffers;
   struct v4l2_control ctl;
   struct v4l2_requestbuffers reqbuf;
   struct v4l2_buffer *bufOut;
   struct v4l2_exportbuffer expbuf;
   void *bufStart;
   int32_t bufferType;

   bufferType= (v4l2->isMultiPlane ? V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE : V4L2_BUF_TYPE_VIDEO_CAPTURE);

   neededBuffers= NUM_OUTPUT_BUFFERS;
   
   memset( &ctl, 0, sizeof(ctl));
   ctl.id= V4L2_CID_MIN_BUFFERS_FOR_CAPTURE;
   rc= IOCTL( v4l2->v4l2Fd, VIDIOC_G_CTRL, &ctl );
   if ( rc == 0 )
   {
      v4l2->minBuffersOut= ctl.value;
      if ( v4l2->minBuffersOut != 0 )
      {
         neededBuffers= v4l2->minBuffersOut+2;
      }
   }

   if ( v4l2->minBuffersOut == 0 )
   {
      v4l2->minBuffersOut= MIN_OUTPUT_BUFFERS;
   }

   memset( &reqbuf, 0, sizeof(reqbuf) );
   reqbuf.count= neededBuffers;
   reqbuf.type= bufferType;
   reqbuf.memory= V4L2_MEMORY_MMAP;
   rc= IOCTL( v4l2->v4l2Fd, VIDIOC_REQBUFS, &reqbuf );
   if ( rc < 0 )
   {
      iprintf(0,"Error: setupOutputBuffers: decoder %d failed to request %d mmap buffers for output: rc %d errno %d\n", v4l2->decCtx->decodeIndex, neededBuffers, rc, errno);
      goto exit;
   }
   v4l2->numBuffersOut= reqbuf.count;

   if ( reqbuf.count < v4l2->minBuffersOut )
   {
      iprintf(0,"Error: setupOutputBuffers: decoder %d insufficient buffers: (%d versus %d)\n", v4l2->decCtx->decodeIndex, reqbuf.count, neededBuffers );
      goto exit;
   }

   v4l2->outBuffers= (BufferInfo*)calloc( reqbuf.count, sizeof(BufferInfo) );
   if ( !v4l2->outBuffers )
   {
      iprintf(0,"Error: setupOutputBuffers: decoder %d no memory for BufferInfo\n", v4l2->decCtx->decodeIndex );
      goto exit;
   }

   for( int i= 0; i < reqbuf.count; ++i )
   {
      v4l2->outBuffers[i].fd= -1;
      for( int j= 0; j < 3; ++j )
      {
         v4l2->outBuffers[i].planeInfo[j].fd= -1;
      }
   }

   for( int i= 0; i < reqbuf.count; ++i )
   {
      bufOut= &v4l2->outBuffers[i].buf;
      bufOut->type= bufferType;
      bufOut->index= i;
      bufOut->memory= V4L2_MEMORY_MMAP;
      if ( v4l2->isMultiPlane )
      {
         memset( v4l2->outBuffers[i].planes, 0, sizeof(struct v4l2_plane)*MAX_PLANES);
         bufOut->m.planes= v4l2->outBuffers[i].planes;
         bufOut->length= v4l2->fmtOut.fmt.pix_mp.num_planes;
      }
      rc= IOCTL( v4l2->v4l2Fd, VIDIOC_QUERYBUF, bufOut );
      if ( rc < 0 )
      {
         iprintf(0,"Error: setupOutputBuffers: decoder %d failed to query input buffer %d: rc %d errno %d\n", v4l2->decCtx->decodeIndex, i, rc, errno);
         goto exit;
      }
      if ( v4l2->isMultiPlane )
      {
         v4l2->outBuffers[i].planeCount= bufOut->length;
         for( int j= 0; j < v4l2->outBuffers[i].planeCount; ++j )
         {
            iprintf(2,"Output buffer: %d\n", i);
            iprintf(2,"  index: %d bytesUsed %d length %d flags %08x\n",
                   bufOut->index, bufOut->m.planes[j].bytesused, bufOut->m.planes[j].length, bufOut->flags );

            memset( &expbuf, 0, sizeof(expbuf) );
            expbuf.type= bufOut->type;
            expbuf.index= i;
            expbuf.plane= j;
            expbuf.flags= O_CLOEXEC;
            rc= IOCTL( v4l2->v4l2Fd, VIDIOC_EXPBUF, &expbuf );
            if ( rc < 0 )
            {
               iprintf(0,"setupOutputBuffers: decoder %d failed to export v4l2 output buffer %d: plane: %d rc %d errno %d\n", v4l2->decCtx->decodeIndex, i, j, rc, errno);
            }
            iprintf(2,"  plane %d index %d export fd %d\n", j, expbuf.index, expbuf.fd );

            v4l2->outBuffers[i].planeInfo[j].fd= expbuf.fd;
            v4l2->outBuffers[i].planeInfo[j].capacity= bufOut->m.planes[j].length;
         }

         /* Use fd of first plane to identify buffer */
         v4l2->outBuffers[i].fd= v4l2->outBuffers[i].planeInfo[0].fd;
      }
      else
      {
         iprintf(2,"Output buffer: %d\n", i);
         iprintf(2,"  index: %d bytesUsed %d length %d flags %08x\n",
                bufOut->index, bufOut->bytesused, bufOut->length, bufOut->flags );

         memset( &expbuf, 0, sizeof(expbuf) );
         expbuf.type= bufOut->type;
         expbuf.index= i;
         expbuf.flags= O_CLOEXEC;
         rc= IOCTL( v4l2->v4l2Fd, VIDIOC_EXPBUF, &expbuf );
         if ( rc < 0 )
         {
            iprintf(0,"setupOutputBuffers: decoder %d failed to export v4l2 output buffer %d: rc %d errno %d\n", v4l2->decCtx->decodeIndex, i, rc, errno);
         }
         iprintf(2,"  index %d export fd %d\n", expbuf.index, expbuf.fd );

         v4l2->outBuffers[i].fd= expbuf.fd;
         v4l2->outBuffers[i].capacity= bufOut->length;
      }
   }

   result= true;

exit:

   if ( !result )
   {
      tearDownOutputBuffers( v4l2 );
   }

   return result;
}

static void tearDownOutputBuffers( V4l2Ctx *v4l2 )
{
   int rc;
   struct v4l2_requestbuffers reqbuf;
   int32_t bufferType;

   bufferType= (v4l2->isMultiPlane ? V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE : V4L2_BUF_TYPE_VIDEO_CAPTURE);

   if ( v4l2->outBuffers )
   {
      rc= IOCTL( v4l2->v4l2Fd, VIDIOC_STREAMOFF, &v4l2->fmtOut.type );
      if ( rc < 0 )
      {
         iprintf(0,"Error: tearDownOutputBuffers: decoder %d streamoff failed for output: rc %d errno %d\n", v4l2->decCtx->decodeIndex, rc, errno );
      }

      for( int i= 0; i < v4l2->numBuffersOut; ++i )
      {
         if ( v4l2->outBuffers[i].planeCount )
         {
            for( int j= 0; j < v4l2->outBuffers[i].planeCount; ++j )
            {
               if ( v4l2->outBuffers[i].planeInfo[j].fd >= 0 )
               {
                  close( v4l2->outBuffers[i].planeInfo[j].fd );
                  v4l2->outBuffers[i].planeInfo[j].fd= -1;
               }
            }
            v4l2->outBuffers[i].fd= -1;
            v4l2->outBuffers[i].planeCount= 0;
         }
         if ( v4l2->outBuffers[i].fd >= 0 )
         {
            close( v4l2->outBuffers[i].fd );
            v4l2->outBuffers[i].fd= -1;
         }
      }

      free( v4l2->outBuffers );
      v4l2->outBuffers= 0;
   }

   if ( v4l2->numBuffersOut )
   {
      memset( &reqbuf, 0, sizeof(reqbuf) );
      reqbuf.count= 0;
      reqbuf.type= bufferType;
      reqbuf.memory= V4L2_MEMORY_MMAP;
      rc= IOCTL( v4l2->v4l2Fd, VIDIOC_REQBUFS, &reqbuf );
      if ( rc < 0 )
      {
         iprintf(0,"Error: tearDownOutputBuffers: decoder %d failed to release v4l2 buffers for output: rc %d errno %d\n", v4l2->decCtx->decodeIndex, rc, errno);
      }
      v4l2->numBuffersOut= 0;
   }
}

static void stopDecoder( V4l2Ctx *v4l2 )
{
   int rc;

   rc= IOCTL( v4l2->v4l2Fd, VIDIOC_STREAMOFF, &v4l2->fmtIn.type );
   if ( rc < 0 )
   {
      iprintf(0,"Error: stopDecoder: decder %d streamoff failed for input: rc %d errno %d\n", v4l2->decCtx->decodeIndex, rc, errno );
   }

   rc= IOCTL( v4l2->v4l2Fd, VIDIOC_STREAMOFF, &v4l2->fmtOut.type );
   if ( rc < 0 )
   {
      iprintf(0,"Error: stopDecoder: decder %d streamoff failed for output: rc %d errno %d\n", v4l2->decCtx->decodeIndex, rc, errno );
   }
}

static bool initV4l2( V4l2Ctx *v4l2 )
{
   bool result= false;
   int rc;
   struct v4l2_exportbuffer eb;

   v4l2->v4l2Fd= open( gDeviceName, O_RDWR );
   iprintf(2,"v4l2Fd %d\n", v4l2->v4l2Fd);
   if ( v4l2->v4l2Fd < 0 )
   {
      iprintf(0,"Error: initV4l2: failed to open device (%s)\n", gDeviceName );
      goto exit;
   }

   rc= IOCTL( v4l2->v4l2Fd, VIDIOC_QUERYCAP, &v4l2->caps );
   if ( rc < 0 )
   {
      iprintf(0,"Error: initV4l2: failed query caps: %d errno %d\n", rc, errno);
      goto exit;
   }

   iprintf(2,"driver (%s) card(%s) bus_info(%s) version %d capabilities %X device_caps %X\n", 
           v4l2->caps.driver, v4l2->caps.card, v4l2->caps.bus_info, v4l2->caps.version, v4l2->caps.capabilities, v4l2->caps.device_caps );

   v4l2->deviceCaps= (v4l2->caps.capabilities & V4L2_CAP_DEVICE_CAPS ) ? v4l2->caps.device_caps : v4l2->caps.capabilities;

   if ( !(v4l2->deviceCaps & (V4L2_CAP_VIDEO_M2M | V4L2_CAP_VIDEO_M2M_MPLANE) ))
   {
      iprintf(0,"Error: initV4l2: device (%s) is not a M2M device\n", gDeviceName );
      goto exit;
   }

   if ( !(v4l2->deviceCaps & V4L2_CAP_STREAMING) )
   {
      iprintf(0,"Error: initV4l2: device (%s) does not support dmabuf: no V4L2_CAP_STREAMING\n", gDeviceName );
      goto exit;
   }

   if ( (v4l2->deviceCaps & V4L2_CAP_VIDEO_M2M_MPLANE) && !(v4l2->deviceCaps & V4L2_CAP_VIDEO_M2M) )
   {
      iprintf(2,"device is multiplane\n");
      v4l2->isMultiPlane= true;
   }

   eb.type= V4L2_BUF_TYPE_VIDEO_CAPTURE;
   eb.index= -1;
   eb.plane= -1;
   eb.flags= (O_RDWR|O_CLOEXEC);
   IOCTL( v4l2->v4l2Fd, VIDIOC_EXPBUF, &eb );
   if ( errno == ENOTTY )
   {
      iprintf(0,"Error: initV4l2: device (%s) does not support dmabuf: no VIDIOC_EXPBUF\n", gDeviceName );
      goto exit;
   }

   v4l2->inputFormat= V4L2_PIX_FMT_H264;

   getInputFormats( v4l2 );

   getOutputFormats( v4l2 );

   setInputFormat( v4l2 );

   setupInputBuffers( v4l2 );

   result= true;

exit:
   return result;   
}

static void termV4l2( V4l2Ctx *v4l2 )
{
   if ( v4l2 )
   {
      tearDownInputBuffers( v4l2 );

      tearDownOutputBuffers( v4l2 );

      if ( v4l2->inputFormats )
      {
         free( v4l2->inputFormats );
      }
      if ( v4l2->outputFormats )
      {
         free( v4l2->outputFormats );
      }
      if ( v4l2->v4l2Fd >= 0 )
      {
         close( v4l2->v4l2Fd );
         v4l2->v4l2Fd= -1;
      }
   }
}

static int getInputBuffer( V4l2Ctx *v4l2 )
{
   int bufferIndex= -1;
   int i;
   for( i= 0; i < v4l2->numBuffersIn; ++i )
   {
      if ( !(v4l2->inBuffers[i].buf.flags & V4L2_BUF_FLAG_QUEUED) ||
           v4l2->inBuffers[i].buf.flags & V4L2_BUF_FLAG_DONE )
      {
         bufferIndex= i;
         break;
      }
   }

   if ( bufferIndex < 0 )
   {
      int rc;
      struct v4l2_buffer buf;
      struct v4l2_plane planes[MAX_PLANES];
      memset( &buf, 0, sizeof(buf));
      buf.type= v4l2->fmtIn.type;
      buf.memory= V4L2_MEMORY_MMAP;
      if ( v4l2->isMultiPlane )
      {
         memset( planes, 0, sizeof(planes));
         buf.length= 1;
         buf.m.planes= planes;
      }
      rc= IOCTL( v4l2->v4l2Fd, VIDIOC_DQBUF, &buf );
      if ( rc == 0 )
      {
         bufferIndex= buf.index;
         if ( v4l2->isMultiPlane )
         {
            memcpy( v4l2->inBuffers[bufferIndex].buf.m.planes, buf.m.planes, sizeof(struct v4l2_plane)*MAX_PLANES);
            buf.m.planes= v4l2->inBuffers[bufferIndex].buf.m.planes;
         }
         v4l2->inBuffers[bufferIndex].buf= buf;
      }
      else if ( !v4l2->decCtx->videoInThreadStopRequested )
      {
         iprintf(0,"Error: getInputBuffer: decoder %d VIDIOC_DQBUF rc %d errno %d\n", v4l2->decCtx->decodeIndex, rc, errno); 
      }
   }

   return bufferIndex;
}

static int getOutputBuffer( V4l2Ctx *v4l2 )
{
   int bufferIndex= -1;
   int rc;
   struct v4l2_buffer buf;
   struct v4l2_plane planes[MAX_PLANES];

   memset( &buf, 0, sizeof(buf));
   buf.type= v4l2->fmtOut.type;
   buf.memory= V4L2_MEMORY_MMAP;
   if ( v4l2->isMultiPlane )
   {
      memset( planes, 0, sizeof(planes));
      buf.length= v4l2->outBuffers[0].planeCount;
      buf.m.planes= planes;
   }
   rc= IOCTL( v4l2->v4l2Fd, VIDIOC_DQBUF, &buf );
   if ( rc == 0 )
   {
      bufferIndex= buf.index;
      if ( v4l2->isMultiPlane )
      {
         memcpy( v4l2->outBuffers[bufferIndex].buf.m.planes, buf.m.planes, sizeof(struct v4l2_plane)*MAX_PLANES);
         buf.m.planes= v4l2->outBuffers[bufferIndex].buf.m.planes;
      }
      v4l2->outBuffers[bufferIndex].buf= buf;
   }

   return bufferIndex;
}

static int findOutputBuffer( V4l2Ctx *v4l2, int fd )
{
   int bufferIndex= -1;
   int i;

   for( i= 0; i < v4l2->numBuffersOut; ++i )
   {
      if ( v4l2->outBuffers[i].fd == fd )
      {
         bufferIndex= i;
         break;
      }
   }

   return bufferIndex;
}

static void *videoEOSThread( void *arg )
{
   DecCtx *decCtx= (DecCtx*)arg;
   V4l2Ctx *v4l2= &decCtx->v4l2;
   int outputFrameCount, count, eosCountDown;

   iprintf(3,"videoEOSThread: enter\n");
   decCtx->videoEOSThreadStarted= true;

   eosCountDown= 1000;
   outputFrameCount= decCtx->outputFrameCount;
   while( !decCtx->videoEOSThreadStopRequested )
   {
      usleep( 1000000/decCtx->videoRate );

      count= decCtx->outputFrameCount;
      if ( outputFrameCount == count )
      {
         --eosCountDown;
         if ( eosCountDown == 0 )
         {
            iprintf(0,"EOS detected decoder %d\n", decCtx->decodeIndex);
            break;
         }
      }
      else
      {
         outputFrameCount= count;
         eosCountDown= 1000;
      }
   }

   decCtx->playing= false;

   decCtx->videoEOSThreadStarted= false;
   iprintf(3,"videoEOSThread: exit\n");

   return 0;
}

static void *videoOutputThread( void *arg )
{
   DecCtx *decCtx= (DecCtx*)arg;
   V4l2Ctx *v4l2= &decCtx->v4l2;
   struct v4l2_selection selection;
   int i, j, buffIndex, rc;
   int32_t bufferType;
   int frameNumber= 0;
   long long prevFrameTime= 0, currFrameTime;

   iprintf(3,"videoOutputThread: enter\n");
   decCtx->videoOutThreadStarted= true;

   for( i= 0; i < v4l2->numBuffersOut; ++i )
   {
      if ( v4l2->isMultiPlane )
      {
         for( j= 0; j < v4l2->outBuffers[i].planeCount; ++j )
         {
            v4l2->outBuffers[i].buf.m.planes[j].bytesused= v4l2->outBuffers[i].buf.m.planes[j].length;
         }
      }
      rc= IOCTL( v4l2->v4l2Fd, VIDIOC_QBUF, &v4l2->outBuffers[i].buf );
      if ( rc < 0 )
      {
         iprintf(0,"Error: videoOutputThread: decoder %d failed to queue output buffer: rc %d errno %d\n", decCtx->decodeIndex, rc, errno);
         decCtx->async->error= true;
         goto exit;
      }
   }

   rc= IOCTL( v4l2->v4l2Fd, VIDIOC_STREAMON, &v4l2->fmtOut.type );
   if ( rc < 0 )
   {
      iprintf(0,"Error: videoOutputThread: decoder %d streamon failed for output: rc %d errno %d\n", decCtx->decodeIndex, rc, errno );
      decCtx->async->error= true;
      goto exit;
   }

   bufferType= v4l2->isMultiPlane ? V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE :V4L2_BUF_TYPE_VIDEO_CAPTURE;
   memset( &selection, 0, sizeof(selection) );
   selection.type= bufferType;
   selection.target= V4L2_SEL_TGT_COMPOSE_DEFAULT;
   rc= IOCTL( v4l2->v4l2Fd, VIDIOC_G_SELECTION, &selection );
   if ( rc < 0 )
   {
      bufferType= V4L2_BUF_TYPE_VIDEO_CAPTURE;
      memset( &selection, 0, sizeof(selection) );
      selection.type= bufferType;
      selection.target= V4L2_SEL_TGT_COMPOSE_DEFAULT;
      rc= IOCTL( v4l2->v4l2Fd, VIDIOC_G_SELECTION, &selection );
      if ( rc < 0 )
      {
         iprintf(0,"Error: videoOutputThread: decoder %d failed to get compose rect: rc %d errno %d\n", decCtx->decodeIndex, rc, errno );
         decCtx->async->error= true;
         goto exit;
      }
   }
   iprintf(2,"Out rect: (%d, %d, %d, %d)\n", selection.r.left, selection.r.top, selection.r.width, selection.r.height );

   pthread_mutex_lock( &decCtx->mutex );
   decCtx->videoWidth= selection.r.width;
   decCtx->videoHeight= selection.r.height;
   iprintf(0,"%lld: decoder %d frame size: %dx%d capture buffer count %d\n", getCurrentTimeMillis(), decCtx->decodeIndex, decCtx->videoWidth, decCtx->videoHeight, decCtx->v4l2.numBuffersOut );
   pthread_mutex_unlock( &decCtx->mutex );

   for( ; ; )
   {
      if ( decCtx->videoOutThreadStopRequested )
      {
         break;
      }
      else
      {
         buffIndex= getOutputBuffer( v4l2 );

         if ( buffIndex >= 0 )
         {
            currFrameTime= getCurrentTimeMillis();
            if ( prevFrameTime )
            {
               long long framePeriod= currFrameTime-prevFrameTime;
               long long nominalFramePeriod= 1000/decCtx->videoRate;
               long long delay= nominalFramePeriod-framePeriod;
               if ( (delay > 2) && (delay <= nominalFramePeriod) )
               {
                  usleep( (delay-1)*1000 );
                  currFrameTime= getCurrentTimeMillis();
               }
            }
            prevFrameTime= currFrameTime;

            if ( decCtx->videoOutThreadStopRequested )
            {
               break;
            }

            pthread_mutex_lock( &decCtx->mutex );
            decCtx->nextFrameFd= v4l2->outBuffers[buffIndex].fd;
            ++frameNumber;
            pthread_mutex_unlock( &decCtx->mutex );
         }

         pthread_mutex_lock( &decCtx->mutex );
         buffIndex= -1;
         if ( decCtx->prevFrameFd >= 0 )
         {
            buffIndex= findOutputBuffer( v4l2, decCtx->prevFrameFd );
            decCtx->prevFrameFd= -1;
         }
         pthread_mutex_unlock( &decCtx->mutex );
      }

      if ( decCtx->videoOutThreadStopRequested ) break;

      if ( buffIndex >= 0 )
      {
         rc= IOCTL( v4l2->v4l2Fd, VIDIOC_QBUF, &v4l2->outBuffers[buffIndex].buf );
         if ( rc < 0 )
         {
            iprintf(0,"Error: decoder %d failed to re-queue output buffer: rc %d errno %d\n", decCtx->decodeIndex, rc, errno);
            decCtx->async->error= true;
            goto exit;
         }
      }
   }

exit:

   decCtx->videoOutThreadStarted= false;
   iprintf(3,"videoOutputThread: exit\n");

   return 0;
}

static void *videoInputThread( void *arg )
{
   DecCtx *decCtx= (DecCtx*)arg;
   V4l2Ctx *v4l2= &decCtx->v4l2;

   iprintf(3,"videoInputThread: enter\n");
   decCtx->videoInThreadStarted= true;

   playFile( decCtx );

   decCtx->videoInThreadStarted= false;
   iprintf(3,"videoInputThread: exit\n");

   return 0;
}

static void *videoDecodeThread( void *arg )
{
   DecCtx *decCtx= (DecCtx*)arg;
   AppCtx *appCtx= decCtx->appCtx;
   EGLCtx *egl= &appCtx->egl;
   GLCtx *gl= &appCtx->gl;
   Surface *surface= decCtx->surface;
   Async *async= decCtx->async;

   iprintf(3,"videoDecodeThread: enter\n");
   decCtx->videoDecodeThreadStarted= true;

   while ( decCtx->playing )
   {
      usleep( 8000 );

      if ( updateFrame( decCtx, surface ) )
      {
         if ( surface )
         {
            surface->dirty= true;
         }
      }

      if ( !decCtx->videoInThreadStopRequested && (decCtx->outputFrameCount == decCtx->numFramesToDecode) )
      {
         iprintf(0,"%lld: decoder %d decoded %d frames\n", getCurrentTimeMillis(), decCtx->decodeIndex, decCtx->outputFrameCount );
         decCtx->videoInThreadStopRequested= true;
      }
      if ( decCtx->videoInThreadStopRequested && !decCtx->videoInThreadStarted )
      {
         pthread_join( decCtx->videoInThreadId, NULL );
         decCtx->videoOutThreadStopRequested= true;
      }
      if ( decCtx->videoOutThreadStopRequested && !decCtx->videoOutThreadStarted )
      {
         if ( surface )
         {
            pthread_mutex_lock( &decCtx->mutex );
            for( int i= 0; i < MAX_TEXTURES; ++i )
            {
               if ( surface->eglImage[i] )
               {
                  gl->eglDestroyImageKHR( egl->eglDisplay, surface->eglImage[i] );
                  surface->eglImage[i]= 0;
               }
               if ( surface->textureId[i] != GL_NONE )
               {
                  glDeleteTextures( 1, &surface->textureId[i] );
                  surface->textureId[i]= GL_NONE;
               }
            }
            pthread_mutex_unlock( &decCtx->mutex );
         }
         break;
      }
   }

   decCtx->videoInThreadStopRequested= true;
   decCtx->videoOutThreadStopRequested= true;
   decCtx->videoEOSThreadStopRequested= true;

   termV4l2( &decCtx->v4l2 );

   if ( decCtx->videoOutThreadStarted )
   {
      pthread_join( decCtx->videoOutThreadId, NULL );
   }

   if ( decCtx->videoEOSThreadStarted )
   {
      pthread_join( decCtx->videoEOSThreadId, NULL );
   }

   decCtx->videoDecodeThreadStarted= false;
   iprintf(3,"videoDecodeThread: exit\n");

   async->done= true;

   return 0;
}

static bool playFile( DecCtx *decCtx )
{
   bool result= false;
   V4l2Ctx *v4l2= &decCtx->v4l2;
   AppCtx *appCtx= decCtx->appCtx;
   Stream *stream= decCtx->stream;
   int frameIndex, frameOffset, frameLength;
   int buffIndex, rc;

   rc= IOCTL( v4l2->v4l2Fd, VIDIOC_STREAMON, &v4l2->fmtIn.type );
   if ( rc < 0 )
   {
      iprintf(0,"Error: streamon failed for input: decoder %d rc %d errno %d\n", decCtx->decodeIndex, rc, errno );
      goto exit;
   }

   frameIndex= 0;

   for( ; ; )
   {
      if ( decCtx->videoInThreadStopRequested )
      {
         break;
      }

      buffIndex= getInputBuffer( v4l2 );

      if (decCtx->videoInThreadStopRequested )
      {
         break;
      }

      if ( buffIndex < 0 )
      {
         iprintf(0,"Error: playFile: decoder %d unable to get input buffer\n", decCtx->decodeIndex);
         decCtx->async->error= true;
         goto exit;
      }

      if ( frameIndex >= stream->streamFrameCount )
      {
         frameIndex= 0;
      }
      frameOffset= stream->streamFrameOffset[frameIndex];
      frameLength= stream->streamFrameLength[frameIndex];

      memcpy( v4l2->inBuffers[buffIndex].start, &stream->streamData[frameOffset], frameLength );
      
      v4l2->inBuffers[buffIndex].buf.bytesused= frameLength;
      if ( v4l2->isMultiPlane )
      {
         v4l2->inBuffers[buffIndex].buf.m.planes[0].bytesused= frameLength;
      }
      v4l2->inBuffers[buffIndex].buf.timestamp = {0};
      rc= IOCTL( v4l2->v4l2Fd, VIDIOC_QBUF, &v4l2->inBuffers[buffIndex].buf );
      if ( rc < 0 )
      {
         iprintf(0,"Error: playFile: queuing input buffer failed: decoder %d rc %d errno %d\n", decCtx->decodeIndex, rc, errno );
         decCtx->async->error= true;
         goto exit;
      }

      if ( !v4l2->outputStarted )
      {
         v4l2->outputStarted= true;

         setOutputFormat( v4l2 );
         setupOutputBuffers( v4l2 );

         decCtx->ready= true;
         for( ; ; )
         {
            if ( decCtx->paused == false ) break;
            usleep( 1000 );
         }

         rc= pthread_create( &v4l2->decCtx->videoOutThreadId, NULL, videoOutputThread, v4l2->decCtx );
         if ( rc )
         {
            iprintf(0,"Error: unable to start video output thread: decoder %d rc %d errno %d\n", decCtx->decodeIndex, rc, errno);
            decCtx->async->error= true;
            goto exit;
         }

         rc= pthread_create( &v4l2->decCtx->videoEOSThreadId, NULL, videoEOSThread, v4l2->decCtx );
         if ( rc )
         {
            iprintf(0,"Error: unable to start video EOS thread: decoder %d rc %d errno %d\n", decCtx->decodeIndex, rc, errno);
            decCtx->async->error= true;
            goto exit;
         }
      }

      ++frameIndex;
   }

   result= true;

exit:

   return result;
}

static bool parseStreamDescriptor( AppCtx *appCtx, Stream *stream, const char *descriptorFilename )
{
   bool result= false;
   FILE *pFile;
   char line[1024];
   char field[1024];
   char *s;
   int foundValueCount= 0;

   pFile= fopen( descriptorFilename, "rt" );
   if ( pFile )
   {
      for( ; ; )
      {
         s= fgets( line, sizeof(line), pFile );
         if ( s )
         {
            if ( sscanf( s, "file: %s", field ) == 1 )
            {
               stream->inputFilename= strdup(field);
               ++foundValueCount;
            }
            else if ( sscanf( s, "frame-size: %s", field ) == 1 )
            {
               int w, h;
               if ( sscanf( field, "%dx%d", &w, &h ) == 2 )
               {
                  stream->videoWidth= w;
                  stream->videoHeight= h;
                  ++foundValueCount;
               }
            }
            else if ( sscanf( s, "frame-rate: %s", field ) == 1 )
            {
               int rate;
               rate= atoi(field);
               if ( rate )
               {
                  stream->videoRate= rate;
                  ++foundValueCount;
               }
            }
         }
         else
         {
            break;
         }
      }
      if ( foundValueCount == 3 )
      {
         result= true;
      }
      fclose( pFile );
   }
   else
   {
      printf("Error: parseStreamDescriptor: unable to open descriptor (%s)\n", descriptorFilename);
   }

return result;
}

static bool prepareStream( AppCtx *appCtx, Stream *stream )
{
   bool result= false;
   FILE *pFile;
   int rc, streamDataLen, lenDidRead;
   int frameNumber, frameStartOffset, i;
   bool firstFrame;
   char *p;

   pFile= fopen( stream->inputFilename, "rb" );
   if ( !pFile )
   {
      iprintf(0,"Error: prepareStream: unable to open input file (%s)\n", stream->inputFilename );
      goto exit;
   }

   rc= fseek( pFile, 0, SEEK_END );
   if ( rc )
   {
      iprintf(0,"Error: prepareStream: unable to seek to end of input file (%s) : %d\n", stream->inputFilename, errno );
      goto exit;
   }

   streamDataLen= ftell( pFile );
   if ( streamDataLen > MAX_STREAM_LEN )
   {
      streamDataLen= MAX_STREAM_LEN;
   }

   rc= fseek( pFile, 0, SEEK_SET );
   if ( rc )
   {
      iprintf(0,"Error: prepareStream: unable to seek to start of input file (%s) : %d\n", stream->inputFilename, errno );
      goto exit;
   }

   stream->streamData= (char*)malloc( streamDataLen );
   if ( !stream->streamData )
   {
      iprintf(0,"Error: prepareStream: unable to allocate %d bytes for stream data\n", streamDataLen );
      goto exit;
   }

   lenDidRead= fread( stream->streamData, 1, streamDataLen, pFile );
   if ( lenDidRead != streamDataLen )
   {
      iprintf(0,"Error: prepareStream: unable to read %d bytes from stream\n", streamDataLen );
      goto exit;
   }

   firstFrame= true;
   frameNumber= 0;
   frameStartOffset= 0;
   p= stream->streamData;
   for( i= 0; i < streamDataLen-4; ++i )
   {
      if ( (p[i] == 0) && (p[i+1] == 0) && (p[i+2] == 0) && (p[i+3] == 1) )
      {
         if ( p[i+4] == 0x67 || p[i+4] == 0x68 )
         {
            continue;
         }
         if ( firstFrame )
         {
            firstFrame= false;
            continue;
         }
         stream->streamFrameOffset[frameNumber]= frameStartOffset;
         stream->streamFrameLength[frameNumber]= (i - frameStartOffset);
         ++frameNumber;
         frameStartOffset= i;
         if ( frameNumber >= MAX_STREAM_FRAMES )
         {
            break;
         }
      }
   }

   stream->streamFrameCount= frameNumber;
   stream->streamDataLen= streamDataLen;
   iprintf(0,"Indexed %d input frames from (%s)\n", frameNumber, stream->inputFilename );

   result= true;

exit:

   if ( !result )
   {
      if ( stream->streamData )
      {
         free( stream->streamData );
         stream->streamData= 0;
      }
   }

   if ( pFile )
   {
      fclose( pFile );
   }

   return result;
}

static bool updateFrame( DecCtx *decCtx, Surface *surface )
{
   AppCtx *appCtx= decCtx->appCtx;
   EGLCtx *egl= &appCtx->egl;
   GLCtx *gl= &appCtx->gl;
   V4l2Ctx *v4l2= &decCtx->v4l2;
   EGLint attr[28];
   int buffIndex;
   bool dirty= false;

   pthread_mutex_lock( &decCtx->mutex );
   if ( decCtx->nextFrameFd != decCtx->currFrameFd )
   {
      dirty= true;
      ++decCtx->outputFrameCount;

      if ( decCtx->nextFrameFd >= 0 )
      {
         if ( surface )
         {
            int fd0, fd1;

            for( int i= 0; i < MAX_TEXTURES; ++i )
            {
               if ( surface->eglImage[i] )
               {
                  gl->eglDestroyImageKHR( egl->eglDisplay, surface->eglImage[i] );
                  surface->eglImage[i]= 0;
               }
            }

            buffIndex= findOutputBuffer( v4l2, decCtx->nextFrameFd );
            if ( buffIndex >= 0 )
            {
               if ( v4l2->isMultiPlane )
               {
                  fd0= v4l2->outBuffers[buffIndex].planeInfo[0].fd;
                  fd1= v4l2->outBuffers[buffIndex].planeInfo[1].fd;
               }
               else
               {
                  fd0= v4l2->outBuffers[buffIndex].fd;
                  fd1= fd0;
               }
            }
            if ( (fd0 >= 0) && (fd1 >= 0) )
            {
               #ifdef GL_OES_EGL_image_external
               int i= 0;
               attr[i++]= EGL_WIDTH;
               attr[i++]= decCtx->videoWidth;
               attr[i++]= EGL_HEIGHT;
               attr[i++]= decCtx->videoHeight;
               attr[i++]= EGL_LINUX_DRM_FOURCC_EXT;
               attr[i++]= DRM_FORMAT_NV12;
               attr[i++]= EGL_DMA_BUF_PLANE0_FD_EXT;
               attr[i++]= fd0;
               attr[i++]= EGL_DMA_BUF_PLANE0_OFFSET_EXT;
               attr[i++]= 0;
               attr[i++]= EGL_DMA_BUF_PLANE0_PITCH_EXT;
               attr[i++]= decCtx->videoWidth;
               attr[i++]= EGL_DMA_BUF_PLANE1_FD_EXT;
               attr[i++]= fd1;
               attr[i++]= EGL_DMA_BUF_PLANE1_OFFSET_EXT;
               attr[i++]= (fd0 != fd1 ? 0 : decCtx->videoWidth*decCtx->videoHeight);
               attr[i++]= EGL_DMA_BUF_PLANE1_PITCH_EXT;
               attr[i++]= decCtx->videoWidth;
               attr[i++]= EGL_YUV_COLOR_SPACE_HINT_EXT;
               attr[i++]= EGL_ITU_REC709_EXT;
               attr[i++]= EGL_SAMPLE_RANGE_HINT_EXT;
               attr[i++]= EGL_YUV_FULL_RANGE_EXT;
               attr[i++]= EGL_NONE;

               surface->eglImage[0]= gl->eglCreateImageKHR( egl->eglDisplay,
                                                       EGL_NO_CONTEXT,
                                                       EGL_LINUX_DMA_BUF_EXT,
                                                       (EGLClientBuffer)NULL,
                                                       attr );
               if ( surface->eglImage[0] == 0 )
               {
                 iprintf(0,"Error: updateFrame: eglCreateImageKHR failed for decoder %d fd %d: errno %X\n", decCtx->decodeIndex, decCtx->currFrameFd, eglGetError());
               }
               if ( surface->textureId[0] != GL_NONE )
               {
                  glDeleteTextures( 1, &surface->textureId[0] );
                  surface->textureId[0]= GL_NONE;
               }

               surface->textureCount= 1;
               surface->haveYUVTextures= false;
               surface->externalImage= true;
               #else
               attr[0]= EGL_WIDTH;
               attr[1]= decCtx->videoWidth;
               attr[2]= EGL_HEIGHT;
               attr[3]= decCtx->videoHeight;
               attr[4]= EGL_LINUX_DRM_FOURCC_EXT;
               attr[5]= DRM_FORMAT_R8;
               attr[6]= EGL_DMA_BUF_PLANE0_FD_EXT;
               attr[7]= decCtx->currFrameFd;
               attr[8]= EGL_DMA_BUF_PLANE0_OFFSET_EXT;
               attr[9]= 0;
               attr[10]= EGL_DMA_BUF_PLANE0_PITCH_EXT;
               attr[11]= decCtx->videoWidth;
               attr[12]= EGL_NONE;

               surface->eglImage[0]= gl->eglCreateImageKHR( egl->eglDisplay,
                                                       EGL_NO_CONTEXT,
                                                       EGL_LINUX_DMA_BUF_EXT,
                                                       (EGLClientBuffer)NULL,
                                                       attr );
               if ( surface->eglImage[0] == 0 )
               {
                 iprintf(0,"Error: updateFrame: eglCreateImageKHR failed for decoder %d fd %d: errno %X\n", decCtx->decodeIndex, decCtx->currFrameFd, eglGetError());
               }
               if ( surface->textureId[0] != GL_NONE )
               {
                  glDeleteTextures( 1, &surface->textureId[0] );
                  surface->textureId[0]= GL_NONE;
               }

               attr[0]= EGL_WIDTH;
               attr[1]= decCtx->videoWidth/2;
               attr[2]= EGL_HEIGHT;
               attr[3]= decCtx->videoHeight/2;
               attr[4]= EGL_LINUX_DRM_FOURCC_EXT;
               attr[5]= DRM_FORMAT_GR88;
               attr[6]= EGL_DMA_BUF_PLANE0_FD_EXT;
               attr[7]= decCtx->currFrameFd;
               attr[8]= EGL_DMA_BUF_PLANE0_OFFSET_EXT;
               attr[9]= decCtx->videoWidth*decCtx->videoHeight;
               attr[10]= EGL_DMA_BUF_PLANE0_PITCH_EXT;
               attr[11]= decCtx->videoWidth;
               attr[12]= EGL_NONE;

               surface->eglImage[1]= gl->eglCreateImageKHR( egl->eglDisplay,
                                                       EGL_NO_CONTEXT,
                                                       EGL_LINUX_DMA_BUF_EXT,
                                                       (EGLClientBuffer)NULL,
                                                       attr );
               if ( surface->eglImage[1] == 0 )
               {
                 iprintf(0,"Error: updateFrame: eglCreateImageKHR failed for decoder %d fd %d: errno %X\n", decCtx->decodeIndex, decCtx->currFrameFd, eglGetError());
               }
               if ( surface->textureId[1] != GL_NONE )
               {
                  glDeleteTextures( 1, &surface->textureId[1] );
                  surface->textureId[1]= GL_NONE;
               }
               surface->textureCount= 2;
               surface->haveYUVTextures= true;
               surface->externalImage= false;
               #endif
            }
         }

         decCtx->prevFrameFd= decCtx->currFrameFd;
         decCtx->currFrameFd= decCtx->nextFrameFd;
      }
   }
   pthread_mutex_unlock( &decCtx->mutex );

   return dirty;
}

static void testDecode( AppCtx *appCtx, int decodeIndex, int numFramesToDecode, Surface *surface, Async *async, Stream *stream )
{
   int rc;
   DecCtx *decCtx= 0;

   async->started= true;

   decCtx= &appCtx->decode[decodeIndex];

   memset( decCtx, 0, sizeof(DecCtx));

   decCtx->appCtx= appCtx;
   decCtx->surface= surface;
   decCtx->async= async;
   decCtx->stream= stream;
   decCtx->decodeIndex= decodeIndex;
   decCtx->videoWidth= stream->videoWidth;
   decCtx->videoHeight= stream->videoHeight;
   decCtx->videoRate= stream->videoRate;
   decCtx->numFramesToDecode= (numFramesToDecode*stream->videoRate/24);
   decCtx->prevFrameFd= -1;
   decCtx->currFrameFd= -1;
   decCtx->nextFrameFd= -1;
   decCtx->nextFrameFd1= -1;
   decCtx->v4l2.decCtx= decCtx;
   decCtx->paused= true;
   pthread_mutex_init( &decCtx->mutex, 0 );

   iprintf(0,"decoder %d to decode %d frames...\n", decodeIndex, decCtx->numFramesToDecode);

   if ( !initV4l2( &decCtx->v4l2 ) )
   {
      iprintf(0,"Error: decoder %d failed to init v4l2\n", decCtx->decodeIndex);
      async->error= true;
      goto exit;
   }

   decCtx->playing= true;
   rc= pthread_create( &decCtx->videoInThreadId, NULL, videoInputThread, decCtx );
   if ( rc )
   {
      iprintf(0,"Error: unable to start video input thread: decoder %d rc %d errno %d\n", decCtx->decodeIndex, rc, errno);
      async->error= true;
      goto exit;
   }

   rc= pthread_create( &decCtx->videoDecodeThreadId, NULL, videoDecodeThread, decCtx );
   if ( rc )
   {
      iprintf(0,"Error: unable to start video decode thread: decoder %d rc %d errno %d\n", decCtx->decodeIndex, rc, errno);
      async->error= true;
      goto exit;
   }

   for( ; ; )
   {
      if ( decCtx->ready ) break;
      usleep( 1000 );
   }

exit:

   if ( async->error )
   {
      async->done= true;
      iprintf(0,"decoder %d done with error\n", decodeIndex);

      decCtx->videoEOSThreadStopRequested= true;
      decCtx->videoOutThreadStopRequested= true;

      termV4l2( &decCtx->v4l2 );

      if ( decCtx->videoOutThreadStarted )
      {
         pthread_join( decCtx->videoOutThreadId, NULL );
      }

      if ( decCtx->videoEOSThreadStarted )
      {
         pthread_join( decCtx->videoEOSThreadId, NULL );
      }
   }

   return;
}

static bool runUntilDone( AppCtx *appCtx )
{
   bool result;
   bool running;
   bool dirty;
   int i, frameCount, minFrame, maxFrame, maxFrameGap;
   double idleTotal= 0.0;
   int numIdleSamples= 0;

   running= false;
   while( !running )
   {
      running= true;
      for( i= 0; i < NUM_DECODE; ++i )
      {
         if ( appCtx->async[i].started && !(appCtx->decode[i].ready || appCtx->async[i].error) )
         {
            running= false;
         }
      }
      usleep( 1000 );
   }

   for( i= 0; i < NUM_DECODE; ++i )
   {
      if ( appCtx->async[i].started )
      {
         appCtx->decode[i].paused= false;
         appCtx->decode[i].startTime= getCurrentTimeMillis();
      }
   }

   maxFrameGap= 0;
   running= true;
   while( running )
   {
      usleep( 16000 );

      glClearColor( 0, 0, 0, 1 );
      glClear( GL_COLOR_BUFFER_BIT );

      running= false;
      dirty= false;
      minFrame= INT_MAX;
      maxFrame= 0;
      for( i= 0; i < NUM_DECODE; ++i )
      {
         if ( appCtx->async[i].started && !appCtx->async[i].error )
         {
            pthread_mutex_lock( &appCtx->decode[i].mutex );

            frameCount= appCtx->decode[i].outputFrameCount*24/appCtx->stream[i].videoRate;
            if ( frameCount < minFrame ) minFrame= frameCount;
            if ( frameCount > maxFrame ) maxFrame= frameCount;

            if ( appCtx->surface[i].eglImage[0] )
            {
               drawSurface( &appCtx->gl, &appCtx->surface[i] );
               appCtx->surface[i].dirty= false;
               dirty= true;
            }
            if ( appCtx->async[i].done )
            {
               if ( appCtx->decode[i].stopTime == 0 )
               {
                  appCtx->decode[i].stopTime= getCurrentTimeMillis();
               }
            }
            else
            {
               running= true;
            }
            pthread_mutex_unlock( &appCtx->decode[i].mutex );
         }
      }
      if ( dirty )
      {
         eglSwapBuffers( appCtx->egl.eglDisplay, appCtx->egl.eglSurface );
      }
      if ( (maxFrame-minFrame) > maxFrameGap ) maxFrameGap= maxFrame-minFrame;

      idleTotal += getCpuIdle();
      ++numIdleSamples;
   }

   if ( maxFrameGap > 8 )
   {
     iprintf(0,"Playback anomaly: gap between decoders: %d frames\n", maxFrameGap);
   }

   if ( numIdleSamples )
   {
     iprintf(0,"Cpu idle: %2.2f\n", (idleTotal/numIdleSamples));
   }

   emitLoadAverage();

   result= true;
   for( i= 0; i < NUM_DECODE; ++i )
   {
      double decodeRate= 0.0;
      if ( appCtx->async[i].started )
      {
         if ( appCtx->async[i].error )
         {
            result= false;
         }
         if ( appCtx->decode[i].stopTime == 0 )
         {
            appCtx->decode[i].stopTime= getCurrentTimeMillis();
         }
         if ( appCtx->decode[i].stopTime > appCtx->decode[i].startTime )
         {
            decodeRate= (double)(appCtx->decode[i].outputFrameCount*1000)/(double)(appCtx->decode[i].stopTime-appCtx->decode[i].startTime);
         }
         iprintf(0,"Decoder %d: target fps: %d mean fps: %f\n", i, appCtx->stream[i].videoRate, decodeRate );
         pthread_mutex_destroy( &appCtx->decode[i].mutex );
      }
   }

   if ( minFrame < appCtx->numFramesToDecode )
   {
      result= false;
   }

   return result;
}

static void discoverVideoDecoder( void )
{
   int rc, len, i, fd;
   bool acceptsCompressed;
   V4l2Ctx v4l2;
   struct v4l2_exportbuffer eb;
   struct dirent *dirent;

   memset( &v4l2, 0, sizeof(v4l2) );

   DIR *dir= opendir("/dev");
   if ( dir )
   {
      for( ; ; )
      {
         fd= -1;

         dirent= readdir( dir );
         if ( dirent == 0 ) break;

         len= strlen(dirent->d_name);
         if ( (len == 1) && !strncmp( dirent->d_name, ".", len) )
         {
            continue;
         }
         else if ( (len == 2) && !strncmp( dirent->d_name, "..", len) )
         {
            continue;
         }

         if ( (len > 5) && !strncmp( dirent->d_name, "video", 5 ) )
         {
            char name[256+10];
            struct v4l2_capability caps;
            uint32_t deviceCaps;

            strcpy( name, "/dev/" );
            strcat( name, dirent->d_name );
            iprintf(0,"checking device: %s\n", name);
            fd= open( name, O_RDWR );
            if ( fd < 0 )
            {
               goto done_check;
            }

            rc= IOCTL( fd, VIDIOC_QUERYCAP, &caps );
            if ( rc < 0 )
            {
               goto done_check;
            }

            deviceCaps= (caps.capabilities & V4L2_CAP_DEVICE_CAPS ) ? caps.device_caps : caps.capabilities;

            if ( !(deviceCaps & (V4L2_CAP_VIDEO_M2M | V4L2_CAP_VIDEO_M2M_MPLANE) ))
            {
               goto done_check;
            }

            if ( !(deviceCaps & V4L2_CAP_STREAMING) )
            {
               goto done_check;
            }

            eb.type= V4L2_BUF_TYPE_VIDEO_CAPTURE;
            eb.index= -1;
            eb.plane= -1;
            eb.flags= (O_RDWR|O_CLOEXEC);
            IOCTL( fd, VIDIOC_EXPBUF, &eb );
            if ( errno == ENOTTY )
            {
               goto done_check;
            }

            v4l2.v4l2Fd= fd;
            if ( (deviceCaps & V4L2_CAP_VIDEO_M2M_MPLANE) && !(deviceCaps & V4L2_CAP_VIDEO_M2M) )
            {
               v4l2.isMultiPlane= true;
            }

            getInputFormats( &v4l2 );
            acceptsCompressed= false;
            for( i= 0; i < v4l2.numInputFormats; ++i)
            {
               if ( v4l2.inputFormats[i].flags & V4L2_FMT_FLAG_COMPRESSED )
               {
                  acceptsCompressed= true;
                  break;
               }
            }

            if ( v4l2.inputFormats )
            {
               free( v4l2.inputFormats );
            }

            if ( !acceptsCompressed )
            {
               goto done_check;
            }

            gDeviceName= strdup(name );
            iprintf(0,"discover decoder: %s\n", gDeviceName);
            iprintf(0,"driver (%s) card(%s) bus_info(%s) version %d capabilities %X device_caps %X\n", 
                    caps.driver, caps.card, caps.bus_info, caps.version, caps.capabilities, caps.device_caps );
            if ( v4l2.isMultiPlane )
            {
               iprintf(0,"device is multiplane\n");
            }
            gLogLevel= 1;
            getInputFormats( &v4l2 );
            getOutputFormats( &v4l2 );
            gLogLevel= 0;
            if ( v4l2.inputFormats )
            {
               free( v4l2.inputFormats );
            }
            if ( v4l2.outputFormats )
            {
               free( v4l2.outputFormats );
            }
            close( fd );
            break;

         done_check:
            if ( fd >= 0 )
            {
               close( fd );
               fd= -1;
            }
         }
      }
      closedir( dir );
   }
}

static void showUsage( void )
{
   printf("Usage:\n");
   printf("v4l2test <options> <input-descr> [input-descr [input-descr [input_descr]]]\n");
   printf("where\n");
   printf(" input-descr is the name of a descriptor file with the format:\n");
   printf(" file: <stream-file-name>\n");
   printf(" frame-size: <width>x<height>\n");
   printf(" frame-rate: <fps>\n");
   printf("\n");
   printf("options are one of:\n");
   printf("--report <reportfilename>\n");
   printf("--devname <devname>\n");
   printf("--window-size <width>x<height> (eg --window-size 640x480)\n");
   printf("--numframes <n>\n" );
   printf("--verbose\n");
   printf("-? : show usage\n");
   printf("\n");
}

#define NUM_FRAMES_TO_DECODE (400)

int main( int argc, const char **argv )
{
   int nRC= -1;
   int argidx;
   AppCtx *appCtx= 0;
   Surface *surface;
   Async *async;
   Stream *stream;
   int rc, i;
   bool testResult;
   const char *reportFilename= 0;
   const char *eglExtensions= 0;
   const char *glExtensions= 0;
   const char *s= 0;
   int numFramesToDecode= NUM_FRAMES_TO_DECODE;
   int decoderIndex;
   int videoWidth, videoHeight;

   appCtx= (AppCtx*)calloc( 1, sizeof(AppCtx) );
   if ( !appCtx )
   {
      iprintf(0,"Error: unable allocate AppCtx\n");
      goto exit;
   }

   appCtx->windowWidth= DEFAULT_WIDTH;
   appCtx->windowHeight= DEFAULT_HEIGHT;
   appCtx->videoWidth= DEFAULT_FRAME_WIDTH;
   appCtx->videoHeight= DEFAULT_FRAME_HEIGHT;
   appCtx->videoRate= DEFAULT_FRAME_RATE;

   argidx= 1;
   while( argidx < argc )
   {
      if ( argv[argidx][0] == '-' )
      {
         int len= strlen( argv[argidx] );
         if ( (len == 2) && !strncmp( argv[argidx], "-?", len) )
         {
            showUsage();
            goto exit;
         }
         else if ( (len == 8) && !strncmp( argv[argidx], "--report", len) )
         {
            ++argidx;
            if ( argidx < argc )
            {
               reportFilename= strdup( argv[argidx] );
            }
         }
         else if ( (len == 9) && !strncmp( argv[argidx], "--devname", len) )
         {
            ++argidx;
            if ( argidx < argc )
            {
               gDeviceName= strdup( argv[argidx] );
            }
         }
         else if ( (len == 13) && !strncmp( argv[argidx], "--window-size", len) )
         {
            ++argidx;
            if ( argidx < argc )
            {
               int w, h;
               if ( sscanf( argv[argidx], "%dx%d", &w, &h ) == 2 )
               {
                  appCtx->windowWidth= w;
                  appCtx->windowHeight= h;
               }
            }
         }
         else if ( (len == 11) && !strncmp( argv[argidx], "--numframes", len) )
         {
            ++argidx;
            if ( argidx < argc )
            {
               int num= atoi(argv[argidx]);
               if ( num )
               {
                  numFramesToDecode= num;
               }
            }
         }
         else if ( (len == 9) && !strncmp( argv[argidx], "--verbose", len) )
         {
            gVerbose= true;
         }
      }
      else if ( !appCtx->stream[0].inputFilename )
      {
         if ( !parseStreamDescriptor( appCtx, &appCtx->stream[0], argv[argidx] ) )
         {
            printf("Error: bad input descriptor: (%s)\n", argv[argidx] );
         }
      }
      else if ( !appCtx->stream[1].inputFilename )
      {
         if ( !parseStreamDescriptor( appCtx, &appCtx->stream[1], argv[argidx] ) )
         {
            printf("Error: bad input descriptor: (%s)\n", argv[argidx] );
         }
      }
      else if ( !appCtx->stream[2].inputFilename )
      {
         if ( !parseStreamDescriptor( appCtx, &appCtx->stream[2], argv[argidx] ) )
         {
            printf("Error: bad input descriptor: (%s)\n", argv[argidx] );
         }
      }
      else if ( !appCtx->stream[3].inputFilename )
      {
         if ( !parseStreamDescriptor( appCtx, &appCtx->stream[3], argv[argidx] ) )
         {
            printf("Error: bad input descriptor: (%s)\n", argv[argidx] );
         }
      }
      ++argidx;
   }

   for( i= 0; i < NUM_DECODE; ++i )
   {
      if ( appCtx->stream[i].inputFilename == 0 )
      {
         if ( i == 0 )
         {
           iprintf(0,"Error: missing input stream file name\n");
            goto exit;      
         }
         else
         {
            appCtx->stream[i].inputFilename= strdup( appCtx->stream[i-1].inputFilename );
            appCtx->stream[i].videoWidth= appCtx->stream[i-1].videoWidth;
            appCtx->stream[i].videoHeight= appCtx->stream[i-1].videoHeight;
            appCtx->stream[i].videoRate= appCtx->stream[i-1].videoRate;
         }
      }
   }   

   appCtx->numFramesToDecode= numFramesToDecode;

   if ( !reportFilename )
   {
      reportFilename= "/tmp/v4l2test-report.txt";
   }

   gReport= fopen( reportFilename, "wt" );

   iprintf(0,"v4l2test v0.1\n");
   iprintf(0,"-----------------------------------------------------------------\n");

   appCtx->platformCtx= PlatfromInit();
   if ( !appCtx->platformCtx )
   {
      iprintf(0,"Error: PlatformInit failed\n");
      goto exit;
   }

   appCtx->egl.appCtx= appCtx;
   appCtx->egl.useWayland= false;
   appCtx->egl.nativeDisplay= PlatformGetEGLDisplayType( appCtx->platformCtx );
   if ( !initEGL( &appCtx->egl ) )
   {
      iprintf(0,"Error: failed to setup EGL\n");
      goto exit;
   }

   appCtx->gl.appCtx= appCtx;
   if ( !initGL( &appCtx->gl ) )
   {
      iprintf(0,"Error: failed to setup GL\n");
      goto exit;
   }

   glClearColor( 0, 0, 0, 1 );
   glClear( GL_COLOR_BUFFER_BIT );
   eglSwapBuffers( appCtx->egl.eglDisplay, appCtx->egl.eglSurface );

   eglExtensions= eglQueryString( appCtx->egl.eglDisplay, EGL_EXTENSIONS );
   if ( eglExtensions )
   {
      if ( strstr( eglExtensions, "EGL_EXT_image_dma_buf_import" ) )
      {
         appCtx->haveDmaBufImport= true;
      }
   }

   glExtensions= (const char *)glGetString(GL_EXTENSIONS);
   if ( glExtensions )
   {
      #ifdef GL_OES_EGL_image_external
      if ( strstr( glExtensions, "GL_OES_EGL_image_external" ) )
      {
         appCtx->haveExternalImage= true;
      }
      #endif
   }

   iprintf(0,"-----------------------------------------------------------------\n");
   iprintf(0,"Have dmabuf import: %d\n", appCtx->haveDmaBufImport );
   iprintf(0,"Have external image: %d\n", appCtx->haveExternalImage );
   iprintf(0,"-----------------------------------------------------------------\n");

   s= eglQueryString( appCtx->egl.eglDisplay, EGL_VENDOR );
   iprintf(0,"EGL_VENDOR: (%s)\n", s );
   s= eglQueryString( appCtx->egl.eglDisplay, EGL_VERSION );
   iprintf(0,"EGL_VERSION: (%s)\n", s );
   s= eglQueryString( appCtx->egl.eglDisplay, EGL_CLIENT_APIS );
   iprintf(0,"EGL_CLIENT_APIS: (%s)\n", s);
   iprintf(0,"EGL_EXTENSIONS: (%s)\n", eglExtensions);
   iprintf(0,"GL_EXTENSIONS: (%s)\n", glExtensions);
   iprintf(0,"-----------------------------------------------------------------\n");

   if ( !appCtx->haveDmaBufImport )
   {
      iprintf(0,"Error: EGL has no dmabuf import support\n");
   }

   if ( !gDeviceName )
   {
      discoverVideoDecoder();
      if ( !gDeviceName )
      {
         iprintf(0,"No v4l2 decoder device found\n");
         goto exit;
      }
   }

   for( i= 0; i < NUM_DECODE; ++i )
   {
      if ( !prepareStream( appCtx, &appCtx->stream[i] ) )
      {
         iprintf(0,"Unable to prepare input stream\n");
         goto exit;
      }
   }

   usleep( 2000000 );

   iprintf(0,"\n");
   iprintf(0,"-----------------------------------------------------------------\n");
   iprintf(0,"Test single decode:\n");

   decoderIndex= 0;
   async= &appCtx->async[decoderIndex];
   async->started= false;
   async->error= false;
   async->done= false;

   videoWidth= appCtx->windowWidth;
   videoHeight= appCtx->windowHeight;

   surface= &appCtx->surface[decoderIndex];
   memset( surface, 0, sizeof(Surface) );
   surface->x= 0;
   surface->y= 0;
   surface->w= videoWidth;
   surface->h= videoHeight;

   stream= &appCtx->stream[decoderIndex];
   testDecode( appCtx, decoderIndex, numFramesToDecode, surface, async, stream );

   testResult= runUntilDone( appCtx );
   iprintf(0,"result: %s\n", testResult ? "PASS" : "FAIL");

   glClearColor( 0, 0, 0, 1 );
   glClear( GL_COLOR_BUFFER_BIT );
   eglSwapBuffers( appCtx->egl.eglDisplay, appCtx->egl.eglSurface );

   iprintf(0,"-----------------------------------------------------------------\n");

   if ( !testResult ) goto exit;

   usleep( 2000000 );



   iprintf(0,"\n");
   iprintf(0,"-----------------------------------------------------------------\n");
   iprintf(0,"Test 2 simultaneous decodes:\n");

   decoderIndex= 0;
   async= &appCtx->async[decoderIndex];
   async->started= false;
   async->error= false;
   async->done= false;

   videoWidth= appCtx->windowWidth/3;
   videoHeight= appCtx->windowHeight/3;

   surface= &appCtx->surface[decoderIndex];
   memset( surface, 0, sizeof(Surface) );
   surface->x= (appCtx->windowWidth-2*videoWidth)/3;
   surface->y= (appCtx->windowHeight-videoHeight)/2;
   surface->w= appCtx->windowWidth/3;
   surface->h= appCtx->windowHeight/3;
   surface->dirty= false;

   stream= &appCtx->stream[decoderIndex];
   testDecode( appCtx, decoderIndex, numFramesToDecode, surface, async, stream );

   ++decoderIndex;
   async= &appCtx->async[decoderIndex];
   async->started= false;
   async->error= false;
   async->done= false;

   surface= &appCtx->surface[decoderIndex];
   memset( surface, 0, sizeof(Surface) );
   surface->x= ((appCtx->windowWidth-2*videoWidth)/3)*2+videoWidth;
   surface->y= (appCtx->windowHeight-videoHeight)/2;
   surface->w= appCtx->windowWidth/3;
   surface->h= appCtx->windowHeight/3;
   surface->dirty= false;

   stream= &appCtx->stream[decoderIndex];
   testDecode( appCtx, decoderIndex, numFramesToDecode, surface, async, stream );

   testResult= runUntilDone( appCtx );
   iprintf(0,"result: %s\n", testResult ? "PASS" : "FAIL");

   glClearColor( 0, 0, 0, 1 );
   glClear( GL_COLOR_BUFFER_BIT );
   eglSwapBuffers( appCtx->egl.eglDisplay, appCtx->egl.eglSurface );
   iprintf(0,"-----------------------------------------------------------------\n");

   if ( !testResult ) goto exit;

   usleep( 2000000 );



   iprintf(0,"\n");
   iprintf(0,"-----------------------------------------------------------------\n");
   iprintf(0,"Test 3 simultaneous decodes:\n");

   decoderIndex= 0;
   async= &appCtx->async[decoderIndex];
   async->started= false;
   async->error= false;
   async->done= false;

   videoWidth= appCtx->windowWidth/3;
   videoHeight= appCtx->windowHeight/3;

   surface= &appCtx->surface[decoderIndex];
   memset( surface, 0, sizeof(Surface) );
   surface->x= (appCtx->windowWidth-2*videoWidth)/3;
   surface->y= (appCtx->windowHeight-2*videoHeight)/3;
   surface->w= appCtx->windowWidth/3;
   surface->h= appCtx->windowHeight/3;
   surface->dirty= false;

   stream= &appCtx->stream[decoderIndex];
   testDecode( appCtx, decoderIndex, numFramesToDecode, surface, async, stream );

   ++decoderIndex;
   async= &appCtx->async[decoderIndex];
   async->started= false;
   async->error= false;
   async->done= false;

   surface= &appCtx->surface[decoderIndex];
   memset( surface, 0, sizeof(Surface) );
   surface->x= ((appCtx->windowWidth-2*videoWidth)/3)*2+videoWidth;
   surface->y= (appCtx->windowHeight-2*videoHeight)/3;
   surface->w= appCtx->windowWidth/3;
   surface->h= appCtx->windowHeight/3;
   surface->dirty= false;

   stream= &appCtx->stream[decoderIndex];
   testDecode( appCtx, decoderIndex, numFramesToDecode, surface, async, stream );

   ++decoderIndex;
   async= &appCtx->async[decoderIndex];
   async->started= false;
   async->error= false;
   async->done= false;

   surface= &appCtx->surface[decoderIndex];
   memset( surface, 0, sizeof(Surface) );
   surface->x= ((appCtx->windowWidth-videoWidth)/2);
   surface->y= ((appCtx->windowHeight-2*videoHeight)/3)*2+videoHeight;
   surface->w= appCtx->windowWidth/3;
   surface->h= appCtx->windowHeight/3;
   surface->dirty= false;

   stream= &appCtx->stream[decoderIndex];
   testDecode( appCtx, decoderIndex, numFramesToDecode, surface, async, stream );

   testResult= runUntilDone( appCtx );
   iprintf(0,"result: %s\n", testResult ? "PASS" : "FAIL");

   glClearColor( 0, 0, 0, 1 );
   glClear( GL_COLOR_BUFFER_BIT );
   eglSwapBuffers( appCtx->egl.eglDisplay, appCtx->egl.eglSurface );
   iprintf(0,"-----------------------------------------------------------------\n");

   if ( !testResult ) goto exit;

   usleep( 2000000 );



   iprintf(0,"\n");
   iprintf(0,"-----------------------------------------------------------------\n");
   iprintf(0,"Test 4 simultaneous decodes:\n");

   decoderIndex= 0;
   async= &appCtx->async[decoderIndex];
   async->started= false;
   async->error= false;
   async->done= false;

   videoWidth= appCtx->windowWidth/3;
   videoHeight= appCtx->windowHeight/3;

   surface= &appCtx->surface[decoderIndex];
   memset( surface, 0, sizeof(Surface) );
   surface->x= (appCtx->windowWidth-2*videoWidth)/3;
   surface->y= (appCtx->windowHeight-2*videoHeight)/3;
   surface->w= appCtx->windowWidth/3;
   surface->h= appCtx->windowHeight/3;
   surface->dirty= false;

   stream= &appCtx->stream[decoderIndex];
   testDecode( appCtx, decoderIndex, numFramesToDecode, surface, async, stream );

   ++decoderIndex;
   async= &appCtx->async[decoderIndex];
   async->started= false;
   async->error= false;
   async->done= false;

   surface= &appCtx->surface[decoderIndex];
   memset( surface, 0, sizeof(Surface) );
   surface->x= ((appCtx->windowWidth-2*videoWidth)/3)*2+videoWidth;
   surface->y= (appCtx->windowHeight-2*videoHeight)/3;
   surface->w= appCtx->windowWidth/3;
   surface->h= appCtx->windowHeight/3;
   surface->dirty= false;

   stream= &appCtx->stream[decoderIndex];
   testDecode( appCtx, decoderIndex, numFramesToDecode, surface, async, stream );

   ++decoderIndex;
   async= &appCtx->async[decoderIndex];
   async->started= false;
   async->error= false;
   async->done= false;

   surface= &appCtx->surface[decoderIndex];
   memset( surface, 0, sizeof(Surface) );
   surface->x= (appCtx->windowWidth-2*videoWidth)/3;
   surface->y= ((appCtx->windowHeight-2*videoHeight)/3)*2+videoHeight;
   surface->w= appCtx->windowWidth/3;
   surface->h= appCtx->windowHeight/3;
   surface->dirty= false;

   stream= &appCtx->stream[decoderIndex];
   testDecode( appCtx, decoderIndex, numFramesToDecode, surface, async, stream );

   ++decoderIndex;
   async= &appCtx->async[decoderIndex];
   async->started= false;
   async->error= false;
   async->done= false;

   surface= &appCtx->surface[decoderIndex];
   memset( surface, 0, sizeof(Surface) );
   surface->x= ((appCtx->windowWidth-2*videoWidth)/3)*2+videoWidth;
   surface->y= ((appCtx->windowHeight-2*videoHeight)/3)*2+videoHeight;
   surface->w= appCtx->windowWidth/3;
   surface->h= appCtx->windowHeight/3;
   surface->dirty= false;

   stream= &appCtx->stream[decoderIndex];
   testDecode( appCtx, decoderIndex, numFramesToDecode, surface, async, stream );

   testResult= runUntilDone( appCtx );
   iprintf(0,"result: %s\n", testResult ? "PASS" : "FAIL");

   glClearColor( 0, 0, 0, 1 );
   glClear( GL_COLOR_BUFFER_BIT );
   eglSwapBuffers( appCtx->egl.eglDisplay, appCtx->egl.eglSurface );
   iprintf(0,"-----------------------------------------------------------------\n");

   if ( !testResult ) goto exit;

exit:

   printf("\n");
   printf("writing report to %s\n", reportFilename );

   if ( appCtx )
   {
      for( i= 0; i < NUM_DECODE; ++i )
      {
         if ( appCtx->stream[i].streamData )
         {
            free( appCtx->stream[i].streamData );
            appCtx->stream[i].streamData= 0;
         }
         if ( appCtx->stream[i].inputFilename )
         {
            free( appCtx->stream[i].inputFilename );
            appCtx->stream[i].inputFilename= 0;
         }
      }

      termGL( &appCtx->gl );

      termEGL( &appCtx->egl );

      if ( appCtx->platformCtx )
      {
         PlatformTerm( appCtx->platformCtx );
         appCtx->platformCtx= 0;
      }

      free( appCtx );
   }

   if ( gDeviceName )
   {
      free( gDeviceName );
      gDeviceName= 0;
   }

   if ( gReport )
   {
      fclose( gReport );
      gReport= 0;
   }

   return nRC;
}


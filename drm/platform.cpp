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
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <memory.h>
#include <pthread.h>
#include <sys/file.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>

#include <EGL/egl.h>
#include <EGL/eglext.h>

#include <gbm.h>
#include <xf86drm.h>
#include <xf86drmMode.h>
#include <drm/drm_fourcc.h>

#include "platform.h"

#include <vector>

typedef EGLBoolean (*PREALEGLSWAPBUFFERS)(EGLDisplay, EGLSurface surface );
typedef EGLSurface (*PREALEGLCREATEWINDOWSURFACE)(EGLDisplay, 
                                                  EGLConfig,
                                                  EGLNativeWindowType,
                                                  const EGLint *attrib_list);

typedef struct _PlatformCtx
{
   pthread_mutex_t mutex;
   int drmFd;
   drmModeRes *res;
   drmModeConnector *conn;
   drmModeEncoder *enc;
   drmModeCrtc *crtc;
   drmModeModeInfo *modeInfo;
   struct gbm_device* gbm;
   bool modeSet;
   void *nativeWindow;
   EGLSurface surfaceDirect;
   uint32_t handle;
   uint32_t fbId;
   int flipPending;
   struct gbm_bo *prevBo;
   uint32_t prevFbId;
} PlatformCtx;

static PREALEGLSWAPBUFFERS gRealEGLSwapBuffers= 0;
static PREALEGLCREATEWINDOWSURFACE gRealEGLCreateWindowSurface= 0;
static PlatformCtx *gCtx= 0;
static bool emitFPS= false;

static void pageFlipEventHandler(int fd, unsigned int frame,
				 unsigned int sec, unsigned int usec,
				 void *data);

PlatformCtx* PlatfromInit( void )
{
   PlatformCtx *ctx= 0;
   drmModeRes *res= 0;
   int i, j, k, len;
   uint32_t n;
   const char *card= "/dev/dri/card0";
   drmModeConnector *conn= 0;
   drmModePlaneRes *planeRes= 0;
   drmModePlane *plane= 0;
   drmModeObjectProperties *props= 0;
   drmModePropertyRes *prop= 0;
   int crtc_idx= -1;
   bool error= true;

   if ( getenv("PLATFORM_FPS" ) )
   {
      emitFPS= true;
   }

   ctx= (PlatformCtx*)calloc( 1, sizeof(PlatformCtx) );
   if ( ctx )
   {
      pthread_mutex_init( &ctx->mutex, 0 );
      ctx->drmFd= -1;
      ctx->drmFd= open(card, O_RDWR);
      if ( ctx->drmFd < 0 )
      {
         printf("Error: PlatformInit: failed to open card (%s)\n", card);
         goto exit;
      }
      res= drmModeGetResources( ctx->drmFd );
      if ( !res )
      {
         printf("Error: PlatformInit: failed to get resources from card (%s)\n", card);
         goto exit;
      }
      for( i= 0; i < res->count_connectors; ++i )
      {
         conn= drmModeGetConnector( ctx->drmFd, res->connectors[i] );
         if ( conn )
         {
            if ( conn->count_modes && (conn->connection == DRM_MODE_CONNECTED) )
            {
               break;
            }
            drmModeFreeConnector(conn);
            conn= 0;
         }
      }
      if ( !conn )
      {
         printf("Error: PlatformInit: unable to get connector for card (%s)\n", card);
         goto exit;
      }
      ctx->res= res;
      ctx->conn= conn;
      ctx->gbm= gbm_create_device( ctx->drmFd );
      if ( !ctx->gbm )
      {
         printf("Error: PlatformInit: unable to create gbm device for card (%s)\n", card);
         goto exit;
      }
      for( i= 0; i < res->count_encoders; ++i )
      {
         uint32_t crtcId= 0;
         bool found= false;
         ctx->enc= drmModeGetEncoder(ctx->drmFd, res->encoders[i]);
         if ( ctx->enc && (ctx->enc->encoder_id == conn->encoder_id) )
         {
            found= true;
            break;
         } 
         for( j= 0; j < res->count_crtcs; j++ )
         {
            if ( ctx->enc->possible_crtcs & (1 << j))
            {
               crtcId= res->crtcs[j];
               for( k= 0; k < res->count_crtcs; k++ )
               {
                  if ( res->crtcs[k] == crtcId )
                  {
                     drmModeFreeEncoder( ctx->enc );
                     ctx->enc= drmModeGetEncoder(ctx->drmFd, res->encoders[k]);
                     ctx->enc->crtc_id= crtcId;
                     found= true;
                     break;
                  }
               }
               if ( found )
               {
                  break;
               } 
            }
         } 
         if ( !found )
         {
            drmModeFreeEncoder( ctx->enc );
            ctx->enc= 0;
         }
      }
      if ( ctx->enc )
      {
         ctx->crtc= drmModeGetCrtc(ctx->drmFd, ctx->enc->crtc_id);
         if ( ctx->crtc && ctx->crtc->mode_valid )
         {
            printf("PlatformInit: current mode %dx%d@%d\n", ctx->crtc->mode.hdisplay, ctx->crtc->mode.vdisplay, ctx->crtc->mode.vrefresh );

            for( j= 0; j < res->count_crtcs; ++j )
            {
               drmModeCrtc *crtcTest= drmModeGetCrtc( ctx->drmFd, res->crtcs[j] );
               if ( crtcTest )
               {
                  if ( crtcTest->crtc_id == ctx->enc->crtc_id )
                  {
                     crtc_idx= j;
                  }
                  drmModeFreeCrtc( crtcTest );
                  if ( crtc_idx >= 0 )
                  {
                     break;
                  }
               }
            }
         }
         else
         {
            printf("Warning: PlatformInit: unable to determine current mode for connector %p on card %s\n", conn, card);
            for( j= 0; j < res->count_crtcs; ++j )
            {
               drmModeCrtc *crtcTest= drmModeGetCrtc( ctx->drmFd, res->crtcs[j] );
               if ( crtcTest )
               {
                  if ( crtcTest->crtc_id == ctx->enc->crtc_id )
                  {
                     crtc_idx= j;
                  }
                  drmModeFreeCrtc( crtcTest );
                  if ( crtc_idx >= 0 )
                  {
                     break;
                  }
               }
            }
         }
      }
      else
      {
         printf("Error: PlatformInit: did not find encoder\n");
      }
   }

   gRealEGLSwapBuffers= (PREALEGLSWAPBUFFERS)dlsym( RTLD_NEXT, "eglSwapBuffers" );
   if ( !gRealEGLSwapBuffers )
   {
      printf("Error: PlatformInit: unable to locate underlying eglSwapBuffers\n");
      goto exit;
   }

   gRealEGLCreateWindowSurface= (PREALEGLCREATEWINDOWSURFACE)dlsym( RTLD_NEXT, "eglCreateWindowSurface" );
   if ( !gRealEGLCreateWindowSurface )
   {
      printf("Error: PlatformInit: unable to locate underlying eglCreateWindowSurface\n");
      goto exit;
   }

   gCtx= ctx;

   error= false;

exit:
   if ( error )
   {
      PlatformTerm(ctx);
      ctx= 0;
   }

   return ctx;
}

void PlatformTerm( PlatformCtx *ctx )
{
   if ( ctx )
   {
      if ( ctx->gbm )
      {
         gbm_device_destroy(ctx->gbm);
         ctx->gbm= 0;
      }
      if ( ctx->crtc )
      {
         drmModeFreeCrtc(ctx->crtc);
         ctx->crtc= 0;
      }
      if ( ctx->enc )
      {
         drmModeFreeEncoder(ctx->enc);
         ctx->enc= 0;
      }
      if ( ctx->conn )
      {
         drmModeFreeConnector(ctx->conn);
         ctx->conn= 0;
      }
      if ( ctx->res )
      {
         drmModeFreeResources(ctx->res);
         ctx->res= 0;
      }
      if ( ctx->drmFd >= 0 )
      {
         close( ctx->drmFd );
         ctx->drmFd= -1;
      }
      pthread_mutex_destroy( &ctx->mutex );
      free( ctx );
      gCtx= 0;
   }
}

NativeDisplayType PlatformGetEGLDisplayType( PlatformCtx *ctx )
{
   NativeDisplayType displayType;

   displayType= (NativeDisplayType)ctx->gbm;

   return displayType;
}

EGLDisplay PlatformGetEGLDisplay( PlatformCtx *ctx, NativeDisplayType type )
{
   EGLDisplay dpy= EGL_NO_DISPLAY;
   PFNEGLGETPLATFORMDISPLAYEXTPROC realEGLGetPlatformDisplay= 0;
   realEGLGetPlatformDisplay= (PFNEGLGETPLATFORMDISPLAYEXTPROC)eglGetProcAddress( "eglGetPlatformDisplayEXT" );
   if ( realEGLGetPlatformDisplay )
   {
      dpy= realEGLGetPlatformDisplay( EGL_PLATFORM_GBM_KHR, ctx->gbm, NULL );
   }
   else
   {
      dpy= eglGetDisplay( (NativeDisplayType)ctx->gbm );
   }

   return dpy;
}

EGLDisplay PlatformGetEGLDisplayWayland( PlatformCtx *ctx, struct wl_display *display )
{
   EGLDisplay dpy= EGL_NO_DISPLAY;

   dpy= eglGetDisplay( (NativeDisplayType)display );

   return dpy;
}

void *PlatformCreateNativeWindow( PlatformCtx *ctx, int width, int height )
{
   void *nativeWindow= 0;

   if ( ctx )
   {
      bool found= false;
      int i;
      for( i= 0; i < ctx->conn->count_modes; ++i )
      {
         if ( (ctx->conn->modes[i].hdisplay == width) &&
              (ctx->conn->modes[i].vdisplay == height) &&
              (ctx->conn->modes[i].type & DRM_MODE_TYPE_DRIVER) )
         {
            found= true;
            ctx->modeInfo= &ctx->conn->modes[i];
            break;
         }
      }
      if ( !found )
      {
         ctx->modeInfo= &ctx->conn->modes[0];
      }

      nativeWindow= gbm_surface_create(ctx->gbm,
                                       width, height,
                                       GBM_FORMAT_ARGB8888,
                                       GBM_BO_USE_SCANOUT | GBM_BO_USE_RENDERING );

      ctx->nativeWindow= nativeWindow;
   }
   
   return nativeWindow;   
}

void PlatformDestroyNativeWindow( PlatformCtx *ctx, void *nativeWindow )
{
   if ( ctx )
   {
      struct gbm_surface *gs = (struct gbm_surface*)nativeWindow;
      if ( ctx->prevBo )
      {
         gbm_surface_release_buffer(gs, ctx->prevBo);
         drmModeRmFB( ctx->drmFd, ctx->prevFbId );
         ctx->prevBo= 0;
         ctx->prevFbId= 0;
         ctx->modeSet= false;
      }
      gbm_surface_destroy( gs );
      ctx->nativeWindow= 0;
   }
}

static void pageFlipEventHandler(int fd, unsigned int frame,
				 unsigned int sec, unsigned int usec,
				 void *data)
{
   PlatformCtx *ctx= (PlatformCtx*)data;
   if ( ctx->flipPending )
   {
      --ctx->flipPending;
   }
}

EGLAPI EGLBoolean eglSwapBuffers( EGLDisplay dpy, EGLSurface surface )
{
   EGLBoolean result= EGL_FALSE;

   if ( gRealEGLSwapBuffers )
   {
      struct gbm_surface* gs;
      struct gbm_bo *bo;
      uint32_t handle, stride;
      fd_set fds;
      drmEventContext ev;
      int rc;

      result= gRealEGLSwapBuffers( dpy, surface );

      if ( surface == gCtx->surfaceDirect )
      {
         gs= (struct gbm_surface*)gCtx->nativeWindow;
         if ( gs )
         {
            bo= gbm_surface_lock_front_buffer(gs);

            handle= gbm_bo_get_handle(bo).u32;
            stride = gbm_bo_get_stride(bo);

            if ( gCtx->handle != handle )
            {
               rc= drmModeAddFB( gCtx->drmFd,
                                 gCtx->modeInfo->hdisplay,
                                 gCtx->modeInfo->vdisplay,
                                 32,
                                 32,
                                 stride,
                                 handle,
                                 &gCtx->fbId );
                if ( rc )
                {
                   printf("Error: swapBuffers: drmModeAddFB rc %d errno %d\n", rc, errno);
                   goto exit;
                }
                gCtx->handle= handle;
            }

            if ( !gCtx->modeSet )
            {
               rc= drmModeSetCrtc( gCtx->drmFd,
                                   gCtx->enc->crtc_id,
                                   gCtx->fbId,
                                   0,
                                   0,
                                   &gCtx->conn->connector_id,
                                   1,
                                   gCtx->modeInfo );
                if ( rc )
                {
                   printf("Error: swapBuffers: drmModeSetCrtc: rc %d errno %d\n", rc, errno);
                   goto exit;
                }
                gCtx->modeSet= true;
            }
            else
            {
               FD_ZERO(&fds);
               memset(&ev, 0, sizeof(ev));

               rc= drmModePageFlip( gCtx->drmFd,
                                    gCtx->enc->crtc_id,
                                    gCtx->fbId,
                                    DRM_MODE_PAGE_FLIP_EVENT,
                                    gCtx );
               if ( !rc )
               {
                  gCtx->flipPending++;
                  ev.version= 2;
                  ev.page_flip_handler= pageFlipEventHandler;
                  FD_SET(0, &fds);
                  FD_SET(gCtx->drmFd, &fds);
                  rc= select( gCtx->drmFd+1, &fds, NULL, NULL, NULL );
                  if ( rc >= 0 )
                  {
                     if ( FD_ISSET(gCtx->drmFd, &fds) )
                     {
                        drmHandleEvent(gCtx->drmFd, &ev);
                     }
                  }
               }
            }
            if ( gCtx->prevBo )
            {
               gbm_surface_release_buffer(gs, gCtx->prevBo);
               drmModeRmFB( gCtx->drmFd, gCtx->prevFbId );
            }
            gCtx->prevBo= bo;
            gCtx->prevFbId= gCtx->fbId;
         }
      }
      if ( emitFPS )
      {
         static int frameCount= 0;
         static long long lastReportTime= -1LL;
         struct timeval tv;
         long long now;
         gettimeofday(&tv,0);
         now= tv.tv_sec*1000LL+(tv.tv_usec/1000LL);
         ++frameCount;
         if ( lastReportTime == -1LL ) lastReportTime= now;
         if ( now-lastReportTime > 5000 )
         {
            double fps= ((double)frameCount*1000)/((double)(now-lastReportTime));
            printf("platform: fps %f\n", fps);
            lastReportTime= now;
            frameCount= 0;
         }
      }
   }

exit:

   return result;    
}

EGLAPI EGLSurface EGLAPIENTRY eglCreateWindowSurface( EGLDisplay dpy, EGLConfig config,
                                                      EGLNativeWindowType win,
                                                      const EGLint *attrib_list )
{
   EGLSurface eglSurface= EGL_NO_SURFACE;

   if ( gRealEGLCreateWindowSurface )
   {
      eglSurface= gRealEGLCreateWindowSurface( dpy, config, win, attrib_list );
      if ( eglSurface != EGL_NO_SURFACE )
      {
         if ( win == (EGLNativeWindowType)gCtx->nativeWindow )
         {
            gCtx->surfaceDirect= eglSurface;
         }
      }
   }

   return eglSurface;
}


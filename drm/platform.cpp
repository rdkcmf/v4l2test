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

typedef struct _PlatformFormatInfo
{
   uint32_t format;
} PlatformFormatInfo;

typedef struct _PlatformOverlayPlane
{
   struct _PlatformOverlayPlane *next;
   struct _PlatformOverlayPlane *prev;
   bool inUse;
   bool supportsVideo;
   bool supportsGraphics;
   bool frameRateMatchingPlane;
   int zOrder;
   uint32_t crtc_id;
   drmModePlane *plane;
   drmModeObjectProperties *planeProps;
   drmModePropertyRes **planePropRes;
   bool dirty;
   bool readyToFlip;
   bool hide;
   bool hidden;
   int formatCount;
   PlatformFormatInfo *formats;
} PlatformOverlayPlane;

typedef struct _PlatformOverlayPlanes
{
   int totalCount;
   int usedCount;
   PlatformOverlayPlane *availHead;
   PlatformOverlayPlane *availTail;
   PlatformOverlayPlane *usedHead;
   PlatformOverlayPlane *usedTail;
   PlatformOverlayPlane *primary;
} PlatformOverlayPlanes;

typedef struct _PlatformCtx
{
   pthread_mutex_t mutex;
   int drmFd;
   drmModeRes *res;
   drmModeConnector *conn;
   drmModeEncoder *enc;
   drmModeCrtc *crtc;
   drmModeModeInfo *modeInfo;
   drmModeObjectProperties *connectorProps;
   drmModePropertyRes **connectorPropRes;
   drmModeObjectProperties *crtcProps;
   drmModePropertyRes **crtcPropRes;
   PlatformOverlayPlanes overlayPlanes;
   struct gbm_device* gbm;
   bool useZPos;
   bool haveAtomic;
   bool graphicsPreferPrimary;
   bool modeSet;
   void *nativeWindow;
   PlatformOverlayPlane *nativeWindowPlane;
   int windowWidth;
   int windowHeight;
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

extern bool gVerbose;

static void pageFlipEventHandler(int fd, unsigned int frame,
				 unsigned int sec, unsigned int usec,
				 void *data);

static void platformReleaseConnectorProperties( PlatformCtx *ctx )
{
   int i;
   if ( ctx->connectorProps )
   {
      if ( ctx->connectorPropRes )
      {
         for( i= 0; i < ctx->connectorProps->count_props; ++i )
         {
            if ( ctx->connectorPropRes[i] )
            {
               drmModeFreeProperty( ctx->connectorPropRes[i] );
               ctx->connectorPropRes[i]= 0;
            }
         }
         free( ctx->connectorPropRes );
         ctx->connectorPropRes= 0;
      }
      drmModeFreeObjectProperties( ctx->connectorProps );
      ctx->connectorProps= 0;
   }
}

static bool platformAcquireConnectorProperties( PlatformCtx *ctx )
{
   bool error= false;
   int i;

   if ( ctx->conn )
   {
      ctx->connectorProps= drmModeObjectGetProperties( ctx->drmFd, ctx->conn->connector_id, DRM_MODE_OBJECT_CONNECTOR );
      if ( ctx->connectorProps )
      {
         ctx->connectorPropRes= (drmModePropertyRes**)calloc( ctx->connectorProps->count_props, sizeof(drmModePropertyRes*) );
         if ( ctx->connectorPropRes )
         {
            for( i= 0; i < ctx->connectorProps->count_props; ++i )
            {
               ctx->connectorPropRes[i]= drmModeGetProperty( ctx->drmFd, ctx->connectorProps->props[i] );
               if ( ctx->connectorPropRes[i] )
               {
                  if ( gVerbose )
                  fprintf(stderr,"connector property %d name (%s) value (%lld)\n",
                        ctx->connectorProps->props[i], ctx->connectorPropRes[i]->name, ctx->connectorProps->prop_values[i] );
               }
               else
               {
                  error= true;
                  break;
               }
            }
         }
         else
         {
            error= true;
         }
      }
      else
      {
         error= true;
      }
      if ( error )
      {
         platformReleaseConnectorProperties( ctx );
         ctx->haveAtomic= false;
      }
   }

   return !error;
}

static void platformReleaseCrtcProperties( PlatformCtx *ctx )
{
   int i;
   if ( ctx->crtcProps )
   {
      if ( ctx->crtcPropRes )
      {
         for( i= 0; i < ctx->crtcProps->count_props; ++i )
         {
            if ( ctx->crtcPropRes[i] )
            {
               drmModeFreeProperty( ctx->crtcPropRes[i] );
               ctx->crtcPropRes[i]= 0;
            }
         }
         free( ctx->crtcPropRes );
         ctx->crtcPropRes= 0;
      }
      drmModeFreeObjectProperties( ctx->crtcProps );
      ctx->crtcProps= 0;
   }
}

static bool platformAcquireCrtcProperties( PlatformCtx *ctx )
{
   bool error= false;
   int i;

   ctx->crtcProps= drmModeObjectGetProperties( ctx->drmFd, ctx->crtc->crtc_id, DRM_MODE_OBJECT_CRTC );
   if ( ctx->crtcProps )
   {
      ctx->crtcPropRes= (drmModePropertyRes**)calloc( ctx->crtcProps->count_props, sizeof(drmModePropertyRes*) );
      if ( ctx->crtcPropRes )
      {
         for( i= 0; i < ctx->crtcProps->count_props; ++i )
         {
            ctx->crtcPropRes[i]= drmModeGetProperty( ctx->drmFd, ctx->crtcProps->props[i] );
            if ( ctx->crtcPropRes[i] )
            {
               if ( gVerbose )
               fprintf(stderr,"crtc property %d name (%s) value (%lld)\n",
                     ctx->crtcProps->props[i], ctx->crtcPropRes[i]->name, ctx->crtcProps->prop_values[i] );
            }
            else
            {
               error= true;
               break;
            }
         }
      }
      else
      {
         error= true;
      }
   }
   else
   {
      error= true;
   }
   if ( error )
   {
      platformReleaseCrtcProperties( ctx );
      ctx->haveAtomic= false;
   }

   return !error;
}

static void platformReleasePlaneProperties( PlatformCtx *ctx, PlatformOverlayPlane *plane )
{
   int i;
   (void)ctx;
   if ( plane->planeProps )
   {
      if ( plane->planePropRes )
      {
         for( i= 0; i < plane->planeProps->count_props; ++i )
         {
            if ( plane->planePropRes[i] )
            {
               drmModeFreeProperty( plane->planePropRes[i] );
               plane->planePropRes[i]= 0;
            }
         }
         free( plane->planePropRes );
         plane->planePropRes= 0;
      }
      drmModeFreeObjectProperties( plane->planeProps );
      plane->planeProps= 0;
   }
}

static bool platformAcquirePlaneProperties( PlatformCtx *ctx, PlatformOverlayPlane *plane )
{
   bool error= false;
   int i, j;

   plane->planeProps= drmModeObjectGetProperties( ctx->drmFd, plane->plane->plane_id, DRM_MODE_OBJECT_PLANE );
   if ( plane->planeProps )
   {
      plane->planePropRes= (drmModePropertyRes**)calloc( plane->planeProps->count_props, sizeof(drmModePropertyRes*) );
      if ( plane->planePropRes )
      {
         for( i= 0; i < plane->planeProps->count_props; ++i )
         {
            plane->planePropRes[i]= drmModeGetProperty( ctx->drmFd, plane->planeProps->props[i] );
            if ( plane->planePropRes[i] )
            {
               if ( gVerbose )
               fprintf(stderr,"plane %d  property %d name (%s) value (%lld) flags(%x)\n",
                     plane->plane->plane_id, plane->planeProps->props[i], plane->planePropRes[i]->name, plane->planeProps->prop_values[i], plane->planePropRes[i]->flags );
               for( j= 0; j < plane->planePropRes[i]->count_enums; ++j )
               {
                  if ( gVerbose )
                  fprintf(stderr,"  enum name (%s) value %llu\n", plane->planePropRes[i]->enums[j].name, plane->planePropRes[i]->enums[j].value );
               }
            }
            else
            {
               error= true;
               break;
            }
         }
      }
      else
      {
         error= true;
      }
   }
   else
   {
      error= true;
   }
   if ( error )
   {
      platformReleasePlaneProperties( ctx, plane );
      ctx->haveAtomic= false;
   }

   return !error;
}

static void platformOverlayAppendUnused( PlatformOverlayPlanes *planes, PlatformOverlayPlane *overlay )
{
   PlatformOverlayPlane *insertAfter= planes->availHead;

   if ( insertAfter )
   {
      if ( overlay->zOrder < insertAfter->zOrder )
      {
         insertAfter= 0;
      }
      while ( insertAfter )
      {
         if ( !insertAfter->next || overlay->zOrder <= insertAfter->next->zOrder )
         {
            break;
         }
         insertAfter= insertAfter->next;
      }
   }

   if ( insertAfter )
   {
      overlay->next= insertAfter->next;
      if ( insertAfter->next )
      {
         insertAfter->next->prev= overlay;
      }
      else
      {
         planes->availTail= overlay;
      }
      insertAfter->next= overlay;
   }
   else
   {
      overlay->next= planes->availHead;
      if ( planes->availHead )
      {
         planes->availHead->prev= overlay;
      }
      else
      {
         planes->availTail= overlay;
      }
      planes->availHead= overlay;
   }
   overlay->prev= insertAfter;
}

static PlatformOverlayPlane *platformOverlayAllocPrimary( PlatformOverlayPlanes *planes )
{
   PlatformOverlayPlane *overlay= 0;

   pthread_mutex_lock( &gCtx->mutex );

   if ( planes->primary )
   {
      if ( !planes->primary->inUse )
      {
         ++planes->usedCount;

         overlay= planes->primary;
         if ( overlay->next )
         {
            overlay->next->prev= overlay->prev;
         }
         else
         {
            planes->availTail= overlay->prev;
         }
         if ( overlay->prev )
         {
            overlay->prev->next= overlay->next;
         }
         else
         {
            planes->availHead= overlay->next;
         }

         overlay->next= 0;
         overlay->prev= planes->usedTail;
         if ( planes->usedTail )
         {
            planes->usedTail->next= overlay;
         }
         else
         {
            planes->usedHead= overlay;
         }
         planes->usedTail= overlay;
         overlay->inUse= true;
      }
      else
      {
         fprintf(stderr,"primary plane already in use\n");
      }
   }
   else
   {
      fprintf(stderr,"no primary plane found\n");
   }

   pthread_mutex_unlock( &gCtx->mutex );

   return overlay;
}

static PlatformOverlayPlane *platformOverlayAlloc( PlatformOverlayPlanes *planes, bool graphics, bool primaryVideo )
{
   PlatformOverlayPlane *overlay= 0;

   pthread_mutex_lock( &gCtx->mutex );

   if (
         (planes->usedCount < planes->totalCount) &&
         (graphics || planes->availHead->supportsVideo)
      )
   {
      if ( graphics )
      {
         overlay= planes->availTail;
         planes->availTail= overlay->prev;
         if ( planes->availTail )
         {
            planes->availTail->next= 0;
         }
         else
         {
            planes->availHead= 0;
         }
      }
      else
      {
         overlay= planes->availHead;
         while( overlay )
         {
            if ( (primaryVideo && overlay->frameRateMatchingPlane) ||
                 (!primaryVideo && !overlay->frameRateMatchingPlane) )
            {
               if ( overlay->next )
               {
                  overlay->next->prev= overlay->prev;
               }
               else
               {
                  planes->availTail= overlay->prev;
               }
               if ( overlay->prev )
               {
                  overlay->prev->next= overlay->next;
               }
               else
               {
                  planes->availHead= overlay->next;
               }
               break;
            }
            else
            {
               overlay= overlay->next;
            }
         }
      }

      if ( overlay )
      {
         ++planes->usedCount;
         overlay->next= 0;
         overlay->prev= planes->usedTail;
         if ( planes->usedTail )
         {
            planes->usedTail->next= overlay;
         }
         else
         {
            planes->usedHead= overlay;
         }
         planes->usedTail= overlay;
         overlay->inUse= true;
      }
   }

   pthread_mutex_unlock( &gCtx->mutex );

   return overlay;
}

static void platformOverlayFree( PlatformOverlayPlanes *planes, PlatformOverlayPlane *overlay )
{
   if ( overlay )
   {
      int i;
      pthread_mutex_lock( &gCtx->mutex );

      overlay->hide= false;
      overlay->hidden= false;
      overlay->inUse= false;
      if ( planes->usedCount <= 0 )
      {
         fprintf(stderr,"PlatformOverlayFree: unmatched free\n");
      }
      --planes->usedCount;
      if ( overlay->next )
      {
         overlay->next->prev= overlay->prev;
      }
      else
      {
         planes->usedTail= overlay->prev;
      }
      if ( overlay->prev )
      {
         overlay->prev->next= overlay->next;
      }
      else
      {
         planes->usedHead= overlay->next;
      }
      overlay->next= 0;
      overlay->prev= 0;
      platformOverlayAppendUnused( planes, overlay );

      pthread_mutex_unlock( &gCtx->mutex );
   }
}

PlatformCtx* PlatfromInit( void )
{
   PlatformCtx *ctx= 0;
   drmModeRes *res= 0;
   int rc;
   int i, j, k, len;
   uint32_t n;
   const char *card= "/dev/dri/card0";
   drmModeConnector *conn= 0;
   drmModePlaneRes *planeRes= 0;
   drmModePlane *plane= 0;
   drmModeObjectProperties *props= 0;
   drmModePropertyRes *prop= 0;
   struct drm_set_client_cap clientCap;
   struct drm_mode_atomic atom;
   int crtc_idx= -1;
   bool error= true;

   if ( getenv("PLATFORM_FPS" ) )
   {
      emitFPS= true;
   }

   ctx= (PlatformCtx*)calloc( 1, sizeof(PlatformCtx) );
   if ( ctx )
   {
      drmVersionPtr drmver= 0;
      pthread_mutex_init( &ctx->mutex, 0 );
      ctx->drmFd= -1;
      ctx->drmFd= open(card, O_RDWR);
      if ( ctx->drmFd < 0 )
      {
         fprintf(stderr,"Error: PlatformInit: failed to open card (%s)\n", card);
         goto exit;
      }

      drmver= drmGetVersion( ctx->drmFd );
      if ( drmver )
      {
         int len;

         if ( gVerbose )
         fprintf(stderr,"PlatformInit: drmGetVersion: %d.%d.%d name (%.*s) date (%.*s) desc (%.*s)\n",
               drmver->version_major, drmver->version_minor, drmver->version_patchlevel,
               drmver->name_len, drmver->name,
               drmver->date_len, drmver->date,
               drmver->desc_len, drmver->desc );

         len= strlen( drmver->name );
         if ( (len == 3) && !strncmp( drmver->name, "vc4", len ) )
         {
            ctx->useZPos= true;
            fprintf(stderr,"using zpos\n");
         }

         drmFreeVersion( drmver );
      }

      clientCap.capability= DRM_CLIENT_CAP_UNIVERSAL_PLANES;
      clientCap.value= 1;
      rc= ioctl( ctx->drmFd, DRM_IOCTL_SET_CLIENT_CAP, &clientCap);
      if ( gVerbose )
      fprintf(stderr,"PlatformInit: DRM_IOCTL_SET_CLIENT_CAP: DRM_CLIENT_CAP_UNIVERSAL_PLANES rc %d\n", rc);

      clientCap.capability= DRM_CLIENT_CAP_ATOMIC;
      clientCap.value= 1;
      rc= ioctl( ctx->drmFd, DRM_IOCTL_SET_CLIENT_CAP, &clientCap);
      if ( gVerbose )
      fprintf(stderr,"PlatformInit: DRM_IOCTL_SET_CLIENT_CAP: DRM_CLIENT_CAP_ATOMIC rc %d\n", rc);
      if ( rc == 0 )
      {
         ctx->haveAtomic= true;
         fprintf(stderr,"PlatformInit: have drm atomic mode setting\n");
      }

      res= drmModeGetResources( ctx->drmFd );
      if ( !res )
      {
         fprintf(stderr,"Error: PlatformInit: failed to get resources from card (%s)\n", card);
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
         fprintf(stderr,"Error: PlatformInit: unable to get connector for card (%s)\n", card);
         goto exit;
      }
      ctx->res= res;
      ctx->conn= conn;
      ctx->gbm= gbm_create_device( ctx->drmFd );
      if ( !ctx->gbm )
      {
         fprintf(stderr,"Error: PlatformInit: unable to create gbm device for card (%s)\n", card);
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
            fprintf(stderr,"PlatformInit: current mode %dx%d@%d\n", ctx->crtc->mode.hdisplay, ctx->crtc->mode.vdisplay, ctx->crtc->mode.vrefresh );

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
            fprintf(stderr,"Warning: PlatformInit: unable to determine current mode for connector %p on card %s\n", conn, card);
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
         fprintf(stderr,"Error: PlatformInit: did not find encoder\n");
      }
   }

   if ( ctx->haveAtomic )
   {
      platformAcquireConnectorProperties( ctx );
   }
   if ( ctx->haveAtomic )
   {
      platformAcquireCrtcProperties( ctx );
   }
   if ( !ctx->haveAtomic )
   {
      platformReleaseConnectorProperties( ctx );
      platformReleaseCrtcProperties( ctx );
   }

   if ( crtc_idx >= 0 )
   {
      bool haveVideoPlanes= false;

      planeRes= drmModeGetPlaneResources( ctx->drmFd );
      if ( planeRes )
      {
         bool isOverlay, isPrimary, isVideo, isGraphics;
         int zpos;

         if ( gVerbose )
         fprintf(stderr,"PlatformInitCtx: planeRes %p count_planes %d\n", planeRes, planeRes->count_planes );
         for( n= 0; n < planeRes->count_planes; ++n )
         {
            plane= drmModeGetPlane( ctx->drmFd, planeRes->planes[n] );
            if ( plane )
            {
               isOverlay= isPrimary= isVideo= isGraphics= false;
               zpos= 0;

               props= drmModeObjectGetProperties( ctx->drmFd, planeRes->planes[n], DRM_MODE_OBJECT_PLANE );
               if ( props )
               {
                  for( j= 0; j < props->count_props; ++j )
                  {
                     prop= drmModeGetProperty( ctx->drmFd, props->props[j] );
                     if ( prop )
                     {
                        if ( plane->possible_crtcs & (1<<crtc_idx) )
                        {
                           len= strlen(prop->name);
                           if ( (len == 4) && !strncmp( prop->name, "type", len) )
                           {
                              if ( props->prop_values[j] == DRM_PLANE_TYPE_PRIMARY )
                              {
                                 isPrimary= true;
                              }
                              else if ( props->prop_values[j] == DRM_PLANE_TYPE_OVERLAY )
                              {
                                 isOverlay= true;
                              }
                           }
                           else if ( (len == 4) && !strncmp( prop->name, "zpos", len) )
                           {
                              zpos= props->prop_values[j];
                              if ( prop->flags & DRM_MODE_PROP_IMMUTABLE )
                              {
                                 ctx->useZPos= false;
                              }
                           }
                        }
                     }
                  }
               }
               if ( isPrimary || isOverlay )
               {
                  PlatformOverlayPlane *newPlane;
                  newPlane= (PlatformOverlayPlane*)calloc( 1, sizeof(PlatformOverlayPlane) );
                  if ( newPlane )
                  {
                     int rc;
                     int pfi;

                     if ( gVerbose )
                     fprintf(stderr,"plane %d count_formats %d\n", plane->plane_id, plane->count_formats);
                     newPlane->formats= (PlatformFormatInfo*)calloc( plane->count_formats, sizeof(PlatformFormatInfo));
                     if ( newPlane->formats )
                     {
                        newPlane->formatCount= plane->count_formats;
                     }
                     else
                     {
                        fprintf(stderr,"No memory for plane formats\n");
                     }
                     for( pfi= 0; pfi < plane->count_formats; ++pfi )
                     {
                        if ( gVerbose )
                        fprintf(stderr,"plane %d format %d: %x (%.*s)\n", plane->plane_id, pfi, plane->formats[pfi], 4, &plane->formats[pfi]);
                        if ( newPlane->formats )
                        {
                           newPlane->formats[pfi].format= plane->formats[pfi];
                        }
                        switch( plane->formats[pfi] )
                        {
                           case DRM_FORMAT_NV12:
                              isVideo= true;
                              if ( !haveVideoPlanes && !isPrimary )
                              {
                                 newPlane->frameRateMatchingPlane= true;
                                 haveVideoPlanes= true;
                              }
                              break;
                           case DRM_FORMAT_ARGB8888:
                              isGraphics= true;
                              break;
                           default:
                              break;
                        }
                     }
                     ++ctx->overlayPlanes.totalCount;
                     newPlane->plane= plane;
                     newPlane->supportsVideo= isVideo;
                     newPlane->supportsGraphics= isGraphics;
                     if ( ctx->useZPos )
                     {
                        newPlane->zOrder= n;
                        if ( isPrimary )
                        {
                           newPlane->zOrder += planeRes->count_planes;
                        }
                        else if ( isGraphics && !isVideo )
                        {
                           newPlane->zOrder += planeRes->count_planes*2;
                        }
                     }
                     else
                     {
                        newPlane->zOrder= n + zpos*16+((isVideo && !isGraphics) ? 0 : 256);
                     }
                     newPlane->inUse= false;
                     newPlane->crtc_id= ctx->enc->crtc_id;
                     if ( gVerbose )
                     fprintf(stderr,"plane zorder %d primary %d overlay %d video %d gfx %d crtc_id %d\n",
                            newPlane->zOrder, isPrimary, isOverlay, isVideo, isGraphics, newPlane->crtc_id);
                     if ( ctx->haveAtomic )
                     {
                        if ( platformAcquirePlaneProperties( ctx, newPlane ) )
                        {
                           if ( isPrimary )
                           {
                              ctx->overlayPlanes.primary= newPlane;
                           }
                        }
                     }
                     platformOverlayAppendUnused( &ctx->overlayPlanes, newPlane );

                     plane= 0;
                  }
                  else
                  {
                     fprintf(stderr,"No memory for WstOverlayPlane\n");
                  }
               }
               if ( prop )
               {
                  drmModeFreeProperty( prop );
               }
               if ( props )
               {
                  drmModeFreeObjectProperties( props );
               }
               if ( plane )
               {
                  drmModeFreePlane( plane );
                  plane= 0;
               }
            }
            else
            {
               fprintf(stderr,"PlatformInitCtx: drmModeGetPlane failed: errno %d\n", errno);
            }
         }
         drmModeFreePlaneResources( planeRes );
      }
      else
      {
         fprintf(stderr,"PlatformInitCtx: drmModePlaneGetResoures failed: errno %d\n", errno );
      }

      fprintf(stderr, "PlatformInitCtx; found %d overlay planes\n", ctx->overlayPlanes.totalCount );

      if (
           haveVideoPlanes &&
           ctx->overlayPlanes.primary &&
           (ctx->overlayPlanes.availHead != ctx->overlayPlanes.primary)
         )
      {
         ctx->graphicsPreferPrimary= true;
      }
   }

   gRealEGLSwapBuffers= (PREALEGLSWAPBUFFERS)dlsym( RTLD_NEXT, "eglSwapBuffers" );
   if ( !gRealEGLSwapBuffers )
   {
      fprintf(stderr,"Error: PlatformInit: unable to locate underlying eglSwapBuffers\n");
      goto exit;
   }

   gRealEGLCreateWindowSurface= (PREALEGLCREATEWINDOWSURFACE)dlsym( RTLD_NEXT, "eglCreateWindowSurface" );
   if ( !gRealEGLCreateWindowSurface )
   {
      fprintf(stderr,"Error: PlatformInit: unable to locate underlying eglCreateWindowSurface\n");
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
      ctx->windowWidth= width;
      ctx->windowHeight= height;

      if ( !ctx->graphicsPreferPrimary )
      {
         ctx->nativeWindowPlane= platformOverlayAlloc( &ctx->overlayPlanes, true, false );
         fprintf(stderr,"plane %p : zorder: %d\n", ctx->nativeWindowPlane, (ctx->nativeWindowPlane ? ctx->nativeWindowPlane->zOrder: -1) );
      }
      else if ( ctx->haveAtomic )
      {
         ctx->nativeWindowPlane= platformOverlayAllocPrimary( &ctx->overlayPlanes );
         fprintf(stderr,"plane %p : primary: zorder: %d\n", ctx->nativeWindowPlane, (ctx->nativeWindowPlane ? ctx->nativeWindowPlane->zOrder: -1) );
      }
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
      if ( ctx->nativeWindowPlane )
      {
         platformOverlayFree( &ctx->overlayPlanes, ctx->nativeWindowPlane );
         ctx->nativeWindowPlane= 0;
      }
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

static void platformAtomicAddProperty( PlatformCtx *ctx, drmModeAtomicReq *req, uint32_t objectId,
                                       int countProps, drmModePropertyRes **propRes, const char *name, uint64_t value )
{
   int rc;
   int i;
   uint32_t propId= 0;

   for( i= 0; i < countProps; ++i )
   {
      if ( !strcmp( name, propRes[i]->name ) )
      {
         propId= propRes[i]->prop_id;
         break;
      }
   }

   if ( propId > 0 )
   {
      if ( gVerbose )
      fprintf(stderr,"platformAtomicAddProperty: objectId %d: %s, %lld\n", objectId, name, value);
      rc= drmModeAtomicAddProperty( req, objectId, propId, value );
      if ( rc < 0 )
      {
         fprintf(stderr,"platformAtomicAddProperty: drmModeAtomicAddProperty fail: obj %d prop %d (%s) value %lld: rc %d errno %d\n", objectId, propId, name, value, rc, errno );
      }
   }
   else
   {
      fprintf(stderr,"platformAtomicAddProperty: skip prop %s\n", name);
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

      if ( gVerbose ) fprintf(stderr,"eglSwapBuffers: start\n");
      result= gRealEGLSwapBuffers( dpy, surface );

      if ( surface == gCtx->surfaceDirect )
      {
         gs= (struct gbm_surface*)gCtx->nativeWindow;
         if ( gs )
         {
            uint32_t flags= 0;
            drmModeAtomicReq *req= 0;
            uint32_t blobId= 0;

            req= drmModeAtomicAlloc();
            if ( !req )
            {
               fprintf(stderr,"Error: swapBuffers: drmModeAtomicAlloc failed, errno %x\n", errno);
               goto exit;
            }

            if ( !gCtx->modeSet )
            {
               flags |= DRM_MODE_ATOMIC_ALLOW_MODESET;
               platformAtomicAddProperty( gCtx, req, gCtx->conn->connector_id,
                                     gCtx->connectorProps->count_props, gCtx->connectorPropRes,
                                     "CRTC_ID", gCtx->crtc->crtc_id );
               rc= drmModeCreatePropertyBlob( gCtx->drmFd, gCtx->modeInfo, sizeof(*gCtx->modeInfo), &blobId );
               if ( rc == 0 )
               {
                  platformAtomicAddProperty( gCtx, req, gCtx->crtc->crtc_id,
                                        gCtx->crtcProps->count_props, gCtx->crtcPropRes,
                                        "MODE_ID", blobId );

                  platformAtomicAddProperty( gCtx, req, gCtx->crtc->crtc_id,
                                        gCtx->crtcProps->count_props, gCtx->crtcPropRes,
                                        "ACTIVE", 1 );
               }
               else
               {
                  fprintf(stderr,"Error: swapBuffers: drmModeCreatePropertyBlob fail: rc %d errno %d\n", rc, errno);
               }
            }

            bo= gbm_surface_lock_front_buffer(gs);

            handle= gbm_bo_get_handle(bo).u32;
            stride = gbm_bo_get_stride(bo);

            if ( gCtx->handle != handle )
            {
               rc= drmModeAddFB( gCtx->drmFd,
                                 gCtx->windowWidth,
                                 gCtx->windowHeight,
                                 32,
                                 32,
                                 stride,
                                 handle,
                                 &gCtx->fbId );
               if ( rc )
               {
                  fprintf(stderr,"Error: swapBuffers: drmModeAddFB rc %d errno %d\n", rc, errno);
                  goto exit;
               }
               gCtx->handle= handle;

               platformAtomicAddProperty( gCtx, req, gCtx->nativeWindowPlane->plane->plane_id,
                                     gCtx->nativeWindowPlane->planeProps->count_props, gCtx->nativeWindowPlane->planePropRes,
                                     "FB_ID", gCtx->fbId );

               platformAtomicAddProperty( gCtx, req, gCtx->nativeWindowPlane->plane->plane_id,
                                     gCtx->nativeWindowPlane->planeProps->count_props, gCtx->nativeWindowPlane->planePropRes,
                                     "CRTC_ID", gCtx->nativeWindowPlane->crtc_id );

               platformAtomicAddProperty( gCtx, req, gCtx->nativeWindowPlane->plane->plane_id,
                                     gCtx->nativeWindowPlane->planeProps->count_props, gCtx->nativeWindowPlane->planePropRes,
                                     "SRC_X", 0 );

               platformAtomicAddProperty( gCtx, req, gCtx->nativeWindowPlane->plane->plane_id,
                                     gCtx->nativeWindowPlane->planeProps->count_props, gCtx->nativeWindowPlane->planePropRes,
                                     "SRC_Y", 0 );

               platformAtomicAddProperty( gCtx, req, gCtx->nativeWindowPlane->plane->plane_id,
                                     gCtx->nativeWindowPlane->planeProps->count_props, gCtx->nativeWindowPlane->planePropRes,
                                     "SRC_W", gCtx->windowWidth<<16 );

               platformAtomicAddProperty( gCtx, req, gCtx->nativeWindowPlane->plane->plane_id,
                                     gCtx->nativeWindowPlane->planeProps->count_props, gCtx->nativeWindowPlane->planePropRes,
                                     "SRC_H", gCtx->windowHeight<<16 );

               platformAtomicAddProperty( gCtx, req, gCtx->nativeWindowPlane->plane->plane_id,
                                     gCtx->nativeWindowPlane->planeProps->count_props, gCtx->nativeWindowPlane->planePropRes,
                                     "CRTC_X", 0 );

               platformAtomicAddProperty( gCtx, req, gCtx->nativeWindowPlane->plane->plane_id,
                                     gCtx->nativeWindowPlane->planeProps->count_props, gCtx->nativeWindowPlane->planePropRes,
                                     "CRTC_Y", 0 );

               platformAtomicAddProperty( gCtx, req, gCtx->nativeWindowPlane->plane->plane_id,
                                     gCtx->nativeWindowPlane->planeProps->count_props, gCtx->nativeWindowPlane->planePropRes,
                                     "CRTC_W", gCtx->modeInfo->hdisplay );

               platformAtomicAddProperty( gCtx, req, gCtx->nativeWindowPlane->plane->plane_id,
                                     gCtx->nativeWindowPlane->planeProps->count_props, gCtx->nativeWindowPlane->planePropRes,
                                     "CRTC_H", gCtx->modeInfo->vdisplay );

               platformAtomicAddProperty( gCtx, req, gCtx->nativeWindowPlane->plane->plane_id,
                                     gCtx->nativeWindowPlane->planeProps->count_props, gCtx->nativeWindowPlane->planePropRes,
                                     "IN_FENCE_FD", -1 );
               if ( gCtx->useZPos )
               {
                  platformAtomicAddProperty( gCtx, req, gCtx->nativeWindowPlane->plane->plane_id,
                                        gCtx->nativeWindowPlane->planeProps->count_props, gCtx->nativeWindowPlane->planePropRes,
                                        "zpos", gCtx->nativeWindowPlane->zOrder );
               }
            }

            if ( req )
            {
               rc= drmModeAtomicCommit( gCtx->drmFd, req, flags, 0 );
               if ( rc )
               {
                  fprintf(stderr,"drmModeAtomicCommit failed: rc %d errno %d\n", rc, errno );
               }
               if ( gVerbose ) fprintf(stderr,"drmModeAtomicCommit: done\n");
               if ( (flags & DRM_MODE_ATOMIC_ALLOW_MODESET) && !rc )
               {
                  fprintf(stderr,"mode set\n");
                  gCtx->modeSet= true;
               }
               drmModeAtomicFree( req );
               if ( blobId )
               {
                  rc= drmModeDestroyPropertyBlob(gCtx->drmFd, blobId);
                  if ( rc )
                  {
                     fprintf(stderr,"drmModeDestroyPropertyBlob failed: rc %d errno %d\n", rc, errno );
                  }
               }
            }

            if ( gCtx->prevBo )
            {
               drmModeRmFB( gCtx->drmFd, gCtx->prevFbId );
               gbm_surface_release_buffer(gs, gCtx->prevBo);
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
            fprintf(stderr,"platform: fps %f\n", fps);
            lastReportTime= now;
            frameCount= 0;
         }
      }
   }

exit:
   if ( gVerbose ) fprintf(stderr,"eglSwapBuffers: end\n");

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


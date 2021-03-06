/*
 * Copyright (C) 2011, Hewlett-Packard Development Company, L.P.
 *   Author: Sebastian Dröge <sebastian.droege@collabora.co.uk>, Collabora Ltd.
 * Copyright (C) 2013, Collabora Ltd.
 *   Author: Sebastian Dröge <sebastian.droege@collabora.co.uk>
 * Copyright (C) 2014-2015, Renesas Electronics Corporation
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation
 * version 2.1 of the License.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301 USA
 *
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <gst/gst.h>
#include <gst/video/gstvideometa.h>
#include <gst/video/gstvideopool.h>
#include <string.h>
#include <unistd.h>             /* getpagesize() */

#include "gstomxvideodec.h"

#ifdef HAVE_MMNGRBUF
#include "gst/allocators/gstdmabuf.h"
#include "mmngr_buf_user_public.h"
#endif
#include "OMXR_Extension_vdcmn.h"

GST_DEBUG_CATEGORY_STATIC (gst_omx_video_dec_debug_category);
#define GST_CAT_DEFAULT gst_omx_video_dec_debug_category

typedef struct _GstOMXMemory GstOMXMemory;
typedef struct _GstOMXMemoryAllocator GstOMXMemoryAllocator;
typedef struct _GstOMXMemoryAllocatorClass GstOMXMemoryAllocatorClass;

struct _GstOMXMemory
{
  GstMemory mem;

  GstOMXBuffer *buf;
};

struct _GstOMXMemoryAllocator
{
  GstAllocator parent;
};

struct _GstOMXMemoryAllocatorClass
{
  GstAllocatorClass parent_class;
};

/* User data and function for release OMX buffer in no-copy mode */
struct GstOMXBufferCallback
{
  GstOMXPort   * out_port;
  GstOMXBuffer * buf;
};

#define GST_OMX_MEMORY_TYPE "openmax"
#define DEFAULT_FRAME_PER_SECOND  30

static GstMemory *
gst_omx_memory_allocator_alloc_dummy (GstAllocator * allocator, gsize size,
    GstAllocationParams * params)
{
  g_assert_not_reached ();
  return NULL;
}

static void
gst_omx_memory_allocator_free (GstAllocator * allocator, GstMemory * mem)
{
  GstOMXMemory *omem = (GstOMXMemory *) mem;

  /* TODO: We need to remember which memories are still used
   * so we can wait until everything is released before allocating
   * new memory
   */

  g_slice_free (GstOMXMemory, omem);
}

static gpointer
gst_omx_memory_map (GstMemory * mem, gsize maxsize, GstMapFlags flags)
{
  GstOMXMemory *omem = (GstOMXMemory *) mem;

  return omem->buf->omx_buf->pBuffer;
}

static void
gst_omx_memory_unmap (GstMemory * mem)
{
}

static GstMemory *
gst_omx_memory_share (GstMemory * mem, gssize offset, gssize size)
{
  g_assert_not_reached ();
  return NULL;
}

GType gst_omx_memory_allocator_get_type (void);
G_DEFINE_TYPE (GstOMXMemoryAllocator, gst_omx_memory_allocator,
    GST_TYPE_ALLOCATOR);

#define GST_TYPE_OMX_MEMORY_ALLOCATOR   (gst_omx_memory_allocator_get_type())
#define GST_IS_OMX_MEMORY_ALLOCATOR(obj) (G_TYPE_CHECK_INSTANCE_TYPE ((obj), GST_TYPE_OMX_MEMORY_ALLOCATOR))

static void
gst_omx_memory_allocator_class_init (GstOMXMemoryAllocatorClass * klass)
{
  GstAllocatorClass *allocator_class;

  allocator_class = (GstAllocatorClass *) klass;

  allocator_class->alloc = gst_omx_memory_allocator_alloc_dummy;
  allocator_class->free = gst_omx_memory_allocator_free;
}

static void
gst_omx_memory_allocator_init (GstOMXMemoryAllocator * allocator)
{
  GstAllocator *alloc = GST_ALLOCATOR_CAST (allocator);

  alloc->mem_type = GST_OMX_MEMORY_TYPE;
  alloc->mem_map = gst_omx_memory_map;
  alloc->mem_unmap = gst_omx_memory_unmap;
  alloc->mem_share = gst_omx_memory_share;

  /* default copy & is_span */

  GST_OBJECT_FLAG_SET (allocator, GST_ALLOCATOR_FLAG_CUSTOM_ALLOC);
}

#ifndef HAVE_MMNGRBUF
static GstMemory *
gst_omx_memory_allocator_alloc (GstAllocator * allocator, GstMemoryFlags flags,
    GstOMXBuffer * buf, gsize offset, gsize size)
{
  GstOMXMemory *mem;

  /* FIXME: We don't allow sharing because we need to know
   * when the memory becomes unused and can only then put
   * it back to the pool. Which is done in the pool's release
   * function
   */
  flags |= GST_MEMORY_FLAG_NO_SHARE;

  mem = g_slice_new (GstOMXMemory);
  /* the shared memory is always readonly */
  gst_memory_init (GST_MEMORY_CAST (mem), flags, allocator, NULL,
      buf->omx_buf->nAllocLen, buf->port->port_def.nBufferAlignment,
      offset, size);

  mem->buf = buf;

  return GST_MEMORY_CAST (mem);
}
#endif

/* Buffer pool for the buffers of an OpenMAX port.
 *
 * This pool is only used if we either passed buffers from another
 * pool to the OMX port or provide the OMX buffers directly to other
 * elements.
 *
 *
 * A buffer is in the pool if it is currently owned by the port,
 * i.e. after OMX_{Fill,Empty}ThisBuffer(). A buffer is outside
 * the pool after it was taken from the port after it was handled
 * by the port, i.e. {Empty,Fill}BufferDone.
 *
 * Buffers can be allocated by us (OMX_AllocateBuffer()) or allocated
 * by someone else and (temporarily) passed to this pool
 * (OMX_UseBuffer(), OMX_UseEGLImage()). In the latter case the pool of
 * the buffer will be overriden, and restored in free_buffer(). Other
 * buffers are just freed there.
 *
 * The pool always has a fixed number of minimum and maximum buffers
 * and these are allocated while starting the pool and released afterwards.
 * They correspond 1:1 to the OMX buffers of the port, which are allocated
 * before the pool is started.
 *
 * Acquiring a buffer from this pool happens after the OMX buffer has
 * been acquired from the port. gst_buffer_pool_acquire_buffer() is
 * supposed to return the buffer that corresponds to the OMX buffer.
 *
 * For buffers provided to upstream, the buffer will be passed to
 * the component manually when it arrives and then unreffed. If the
 * buffer is released before reaching the component it will be just put
 * back into the pool as if EmptyBufferDone has happened. If it was
 * passed to the component, it will be back into the pool when it was
 * released and EmptyBufferDone has happened.
 *
 * For buffers provided to downstream, the buffer will be returned
 * back to the component (OMX_FillThisBuffer()) when it is released.
 */

static GQuark gst_omx_buffer_data_quark = 0;

#define GST_OMX_BUFFER_POOL(pool) ((GstOMXBufferPool *) pool)
typedef struct _GstOMXBufferPool GstOMXBufferPool;
typedef struct _GstOMXBufferPoolClass GstOMXBufferPoolClass;

typedef struct _GstOMXVideoDecBufferData GstOMXVideoDecBufferData;

struct _GstOMXBufferPool
{
  GstVideoBufferPool parent;

  GstElement *element;

  GstCaps *caps;
  gboolean add_videometa;
  GstVideoInfo video_info;

  /* Owned by element, element has to stop this pool before
   * it destroys component or port */
  GstOMXComponent *component;
  GstOMXPort *port;

  /* For handling OpenMAX allocated memory */
  GstAllocator *allocator;

  /* Set from outside this pool */
  /* TRUE if we're currently allocating all our buffers */
  gboolean allocating;

  /* TRUE if the pool is not used anymore */
  gboolean deactivated;

  /* For populating the pool from another one */
  GstBufferPool *other_pool;
  GPtrArray *buffers;

  /* Used during acquire for output ports to
   * specify which buffer has to be retrieved
   * and during alloc, which buffer has to be
   * wrapped
   */
  gint current_buffer_index;

  /* TRUE if the downstream buffer pool can handle
     "videosink_buffer_creation_request" query */
  gboolean vsink_buf_req_supported;
};

struct _GstOMXBufferPoolClass
{
  GstVideoBufferPoolClass parent_class;
};

struct _GstOMXVideoDecBufferData
{
  gboolean already_acquired;

#ifdef HAVE_MMNGRBUF
  gint id_export[GST_VIDEO_MAX_PLANES];
#endif
};

GType gst_omx_buffer_pool_get_type (void);

G_DEFINE_TYPE (GstOMXBufferPool, gst_omx_buffer_pool, GST_TYPE_BUFFER_POOL);

static void gst_omx_buffer_pool_free_buffer (GstBufferPool * bpool,
    GstBuffer * buffer);

static gboolean
gst_omx_buffer_pool_start (GstBufferPool * bpool)
{
  GstOMXBufferPool *pool = GST_OMX_BUFFER_POOL (bpool);

  /* Only allow to start the pool if we still are attached
   * to a component and port */
  GST_OBJECT_LOCK (pool);
  if (!pool->component || !pool->port) {
    GST_OBJECT_UNLOCK (pool);
    return FALSE;
  }
  GST_OBJECT_UNLOCK (pool);

  return
      GST_BUFFER_POOL_CLASS (gst_omx_buffer_pool_parent_class)->start (bpool);
}

static gboolean
gst_omx_buffer_pool_stop (GstBufferPool * bpool)
{
  GstOMXBufferPool *pool = GST_OMX_BUFFER_POOL (bpool);
  gint i = 0;

  /* When not using the default GstBufferPool::GstAtomicQueue then
   * GstBufferPool::free_buffer is not called while stopping the pool
   * (because the queue is empty) */
  for (i = 0; i < pool->buffers->len; i++)
    GST_BUFFER_POOL_CLASS (gst_omx_buffer_pool_parent_class)->release_buffer
        (bpool, g_ptr_array_index (pool->buffers, i));

  /* Remove any buffers that are there */
  g_ptr_array_set_size (pool->buffers, 0);

  if (pool->caps)
    gst_caps_unref (pool->caps);
  pool->caps = NULL;

  pool->add_videometa = FALSE;

  return GST_BUFFER_POOL_CLASS (gst_omx_buffer_pool_parent_class)->stop (bpool);
}

static const gchar **
gst_omx_buffer_pool_get_options (GstBufferPool * bpool)
{
  static const gchar *raw_video_options[] =
      { GST_BUFFER_POOL_OPTION_VIDEO_META, NULL };
  static const gchar *options[] = { NULL };
  GstOMXBufferPool *pool = GST_OMX_BUFFER_POOL (bpool);

  GST_OBJECT_LOCK (pool);
  if (pool->port && pool->port->port_def.eDomain == OMX_PortDomainVideo
      && pool->port->port_def.format.video.eCompressionFormat ==
      OMX_VIDEO_CodingUnused) {
    GST_OBJECT_UNLOCK (pool);
    return raw_video_options;
  }
  GST_OBJECT_UNLOCK (pool);

  return options;
}

static gboolean
gst_omx_buffer_pool_set_config (GstBufferPool * bpool, GstStructure * config)
{
  GstOMXBufferPool *pool = GST_OMX_BUFFER_POOL (bpool);
  GstCaps *caps;

  GST_OBJECT_LOCK (pool);

  if (!gst_buffer_pool_config_get_params (config, &caps, NULL, NULL, NULL))
    goto wrong_config;

  if (caps == NULL)
    goto no_caps;

  if (pool->port && pool->port->port_def.eDomain == OMX_PortDomainVideo
      && pool->port->port_def.format.video.eCompressionFormat ==
      OMX_VIDEO_CodingUnused) {
    GstVideoInfo info;

    /* now parse the caps from the config */
    if (!gst_video_info_from_caps (&info, caps))
      goto wrong_video_caps;

    /* enable metadata based on config of the pool */
    pool->add_videometa =
        gst_buffer_pool_config_has_option (config,
        GST_BUFFER_POOL_OPTION_VIDEO_META);

    pool->video_info = info;
  }

  if (pool->caps)
    gst_caps_unref (pool->caps);
  pool->caps = gst_caps_ref (caps);

  GST_OBJECT_UNLOCK (pool);

  return GST_BUFFER_POOL_CLASS (gst_omx_buffer_pool_parent_class)->set_config
      (bpool, config);

  /* ERRORS */
wrong_config:
  {
    GST_OBJECT_UNLOCK (pool);
    GST_WARNING_OBJECT (pool, "invalid config");
    return FALSE;
  }
no_caps:
  {
    GST_OBJECT_UNLOCK (pool);
    GST_WARNING_OBJECT (pool, "no caps in config");
    return FALSE;
  }
wrong_video_caps:
  {
    GST_OBJECT_UNLOCK (pool);
    GST_WARNING_OBJECT (pool,
        "failed getting geometry from caps %" GST_PTR_FORMAT, caps);
    return FALSE;
  }
}

static GstFlowReturn
gst_omx_buffer_pool_alloc_buffer (GstBufferPool * bpool,
    GstBuffer ** buffer, GstBufferPoolAcquireParams * params)
{
  GstOMXBufferPool *pool = GST_OMX_BUFFER_POOL (bpool);
  GstBuffer *buf;
  GstOMXBuffer *omx_buf;
  GstOMXVideoDec *self;
  self = GST_OMX_VIDEO_DEC (pool->element);

  g_return_val_if_fail (pool->allocating, GST_FLOW_ERROR);

  omx_buf = g_ptr_array_index (pool->port->buffers, pool->current_buffer_index);
  g_return_val_if_fail (omx_buf != NULL, GST_FLOW_ERROR);

  if (pool->other_pool) {
    guint i, n;

    buf = g_ptr_array_index (pool->buffers, pool->current_buffer_index);
    g_assert (pool->other_pool == buf->pool);
    gst_object_replace ((GstObject **) & buf->pool, NULL);

    n = gst_buffer_n_memory (buf);
    for (i = 0; i < n; i++) {
      GstMemory *mem = gst_buffer_peek_memory (buf, i);

      /* FIXME: We don't allow sharing because we need to know
       * when the memory becomes unused and can only then put
       * it back to the pool. Which is done in the pool's release
       * function
       */
      GST_MINI_OBJECT_FLAG_SET (mem, GST_MEMORY_FLAG_NO_SHARE);
    }

    if (pool->add_videometa) {
      GstVideoMeta *meta;

      meta = gst_buffer_get_video_meta (buf);
      if (!meta) {
        gst_buffer_add_video_meta (buf, GST_VIDEO_FRAME_FLAG_NONE,
            GST_VIDEO_INFO_FORMAT (&pool->video_info),
            GST_VIDEO_INFO_WIDTH (&pool->video_info),
            GST_VIDEO_INFO_HEIGHT (&pool->video_info));
      }
    }
  } else {
    gsize offset[4] = { 0, };
    gint stride[4] = { 0, };
    gsize plane_size[4] = { 0, };
#ifndef HAVE_MMNGRBUF
    guint n_planes;
#endif
    gint i;
    GstOMXVideoDecBufferData *vdbuf_data;

    switch (pool->video_info.finfo->format) {
      case GST_VIDEO_FORMAT_I420:
        offset[0] = 0;
        stride[0] = pool->port->port_def.format.video.nStride;
        offset[1] = stride[0] * pool->port->port_def.format.video.nSliceHeight;
        stride[1] = pool->port->port_def.format.video.nStride / 2;
        offset[2] =
            offset[1] +
            stride[1] * (pool->port->port_def.format.video.nSliceHeight / 2);
        stride[2] = pool->port->port_def.format.video.nStride / 2;
        plane_size[0] = pool->port->port_def.format.video.nStride *
            pool->port->port_def.format.video.nFrameHeight;
        plane_size[1] = plane_size[2] = plane_size[0] / 4;

#ifndef HAVE_MMNGRBUF
        n_planes = 3;
#endif
        break;
      case GST_VIDEO_FORMAT_NV12:
        offset[0] = 0;
        stride[0] = pool->port->port_def.format.video.nStride;
        offset[1] = stride[0] * pool->port->port_def.format.video.nSliceHeight;
        stride[1] = pool->port->port_def.format.video.nStride;
        plane_size[0] = pool->port->port_def.format.video.nStride *
            pool->port->port_def.format.video.nFrameHeight;
        plane_size[1] = plane_size[0] / 2;

#ifndef HAVE_MMNGRBUF
        n_planes = 2;
#endif
        break;
      default:
        g_assert_not_reached ();
        break;
    }

    buf = gst_buffer_new ();

#ifndef HAVE_MMNGRBUF
    if (self->use_dmabuf == FALSE)
      for (i = 0; i < n_planes; i++)
        gst_buffer_append_memory (buf,
            gst_omx_memory_allocator_alloc (pool->allocator, 0, omx_buf,
                offset[i], plane_size[i]));
#endif

    g_ptr_array_add (pool->buffers, buf);

    if (pool->add_videometa)
      gst_buffer_add_video_meta_full (buf, GST_VIDEO_FRAME_FLAG_NONE,
          GST_VIDEO_INFO_FORMAT (&pool->video_info),
          GST_VIDEO_INFO_WIDTH (&pool->video_info),
          GST_VIDEO_INFO_HEIGHT (&pool->video_info),
          GST_VIDEO_INFO_N_PLANES (&pool->video_info), offset, stride);

    /* Initialize an already_acquired flag */
    vdbuf_data = g_slice_new (GstOMXVideoDecBufferData);
    vdbuf_data->already_acquired = FALSE;
#ifdef HAVE_MMNGRBUF
    if (self->use_dmabuf)
      for (i = 0; i < GST_VIDEO_MAX_PLANES; i++)
        vdbuf_data->id_export[i] = -1;
#endif

    omx_buf->private_data = (void *) vdbuf_data;
  }

  gst_mini_object_set_qdata (GST_MINI_OBJECT_CAST (buf),
      gst_omx_buffer_data_quark, omx_buf, NULL);

  *buffer = buf;

  pool->current_buffer_index++;

  return GST_FLOW_OK;
}

static void
gst_omx_buffer_pool_free_buffer (GstBufferPool * bpool, GstBuffer * buffer)
{
  GstOMXBufferPool *pool = GST_OMX_BUFFER_POOL (bpool);
  GstOMXBuffer *omx_buf;
  GstOMXVideoDec *self;
  self = GST_OMX_VIDEO_DEC (pool->element);
#ifdef HAVE_MMNGRBUF
  GstOMXVideoDecBufferData *vdbuf_data;
  gint i;
#endif

  /* If the buffers belong to another pool, restore them now */
  GST_OBJECT_LOCK (pool);
  if (pool->other_pool) {
    gst_object_replace ((GstObject **) & buffer->pool,
        (GstObject *) pool->other_pool);
  }
  GST_OBJECT_UNLOCK (pool);

  omx_buf =
      gst_mini_object_get_qdata (GST_MINI_OBJECT_CAST (buffer),
      gst_omx_buffer_data_quark);
#ifdef HAVE_MMNGRBUF
  if (self->use_dmabuf) {
    vdbuf_data = (GstOMXVideoDecBufferData *) omx_buf->private_data;
    for (i = 0; i < GST_VIDEO_MAX_PLANES; i++)
      if (vdbuf_data->id_export[i] >= 0)
        mmngr_export_end_in_user (vdbuf_data->id_export[i]);
  }
#endif
  g_slice_free (GstOMXVideoDecBufferData, omx_buf->private_data);

  gst_mini_object_set_qdata (GST_MINI_OBJECT_CAST (buffer),
      gst_omx_buffer_data_quark, NULL, NULL);

  GST_BUFFER_POOL_CLASS (gst_omx_buffer_pool_parent_class)->free_buffer (bpool,
      buffer);
}

#ifdef HAVE_MMNGRBUF
static GstBuffer *
gst_omx_buffer_pool_request_videosink_buffer_creation (GstOMXBufferPool * pool,
    gint dmabuf_fd[GST_VIDEO_MAX_PLANES], gint stride[GST_VIDEO_MAX_PLANES])
{
  GstQuery *query;
  GValue val = { 0, };
  GstStructure *structure;
  const GValue *value;
  GstBuffer *buffer;
  GArray *dmabuf_array;
  GArray *stride_array;
  gint n_planes;
  gint i;

  g_value_init (&val, G_TYPE_POINTER);
  g_value_set_pointer (&val, (gpointer) pool->allocator);

  dmabuf_array = g_array_new (FALSE, FALSE, sizeof (gint));
  stride_array = g_array_new (FALSE, FALSE, sizeof (gint));

  n_planes = GST_VIDEO_INFO_N_PLANES (&pool->video_info);
  for (i = 0; i < n_planes; i++) {
    g_array_append_val (dmabuf_array, dmabuf_fd[i]);
    g_array_append_val (stride_array, stride[i]);
  }

  structure = gst_structure_new ("videosink_buffer_creation_request",
      "width", G_TYPE_INT, pool->port->port_def.format.video.nFrameWidth,
      "height", G_TYPE_INT, pool->port->port_def.format.video.nFrameHeight,
      "stride", G_TYPE_ARRAY, stride_array,
      "dmabuf", G_TYPE_ARRAY, dmabuf_array,
      "allocator", G_TYPE_POINTER, &val,
      "format", G_TYPE_STRING,
      gst_video_format_to_string (pool->video_info.finfo->format),
      "n_planes", G_TYPE_INT, n_planes, NULL);

  query = gst_query_new_custom (GST_QUERY_CUSTOM, structure);

  GST_DEBUG_OBJECT (pool, "send a videosink_buffer_creation_request query");

  if (!gst_pad_peer_query (GST_VIDEO_DECODER_SRC_PAD (pool->element), query)) {
    GST_ERROR_OBJECT (pool, "videosink_buffer_creation_request query failed");
    return NULL;
  }

  value = gst_structure_get_value (structure, "buffer");
  buffer = gst_value_get_buffer (value);
  if (buffer == NULL) {
    GST_ERROR_OBJECT (pool,
        "could not get a buffer from videosink_buffer_creation query");
    return NULL;
  }

  gst_query_unref (query);

  g_array_free (dmabuf_array, TRUE);
  g_array_free (stride_array, TRUE);

  return buffer;
}
#endif

#ifdef HAVE_MMNGRBUF
static gboolean
gst_omx_buffer_pool_export_dmabuf (GstOMXBufferPool * pool,
    guint phys_addr, gint size, gint boundary, gint * id_export,
    gint * dmabuf_fd)
{
  gint res;

  res =
      mmngr_export_start_in_user (id_export,
      (size + boundary - 1) & ~(boundary - 1), (unsigned long) phys_addr,
      dmabuf_fd);
  if (res != R_MM_OK) {
    GST_ERROR_OBJECT (pool,
        "mmngr_export_start_in_user failed (phys_addr:0x%08x)", phys_addr);
    return FALSE;
  }
  GST_DEBUG_OBJECT (pool,
      "Export dmabuf:%d id_export:%d (phys_addr:0x%08x)", *dmabuf_fd,
      *id_export, phys_addr);

  return TRUE;
}
#endif

static GstFlowReturn
gst_omx_buffer_pool_acquire_buffer (GstBufferPool * bpool,
    GstBuffer ** buffer, GstBufferPoolAcquireParams * params)
{
  GstFlowReturn ret;
  GstOMXBufferPool *pool = GST_OMX_BUFFER_POOL (bpool);
  GstOMXVideoDec *self;
  self = GST_OMX_VIDEO_DEC (pool->element);

  if (pool->port->port_def.eDir == OMX_DirOutput) {
    GstBuffer *buf;
    GstOMXBuffer *omx_buf;
    GstOMXVideoDecBufferData *vdbuf_data;
#ifdef HAVE_MMNGRBUF
    guint n_mem;
#endif

    g_return_val_if_fail (pool->current_buffer_index != -1, GST_FLOW_ERROR);

    buf = g_ptr_array_index (pool->buffers, pool->current_buffer_index);
    g_return_val_if_fail (buf != NULL, GST_FLOW_ERROR);

    omx_buf =
        gst_mini_object_get_qdata (GST_MINI_OBJECT_CAST (buf),
        gst_omx_buffer_data_quark);

    vdbuf_data = (GstOMXVideoDecBufferData *) omx_buf->private_data;
#ifdef HAVE_MMNGRBUF
    if (self->use_dmabuf)
    {
      n_mem = gst_buffer_n_memory (buf);
      if (n_mem == 0) {
        GstBuffer *new_buf;
        GstVideoMeta *vmeta;
        gint n_planes;
        gint i;
        gint dmabuf_fd[GST_VIDEO_MAX_PLANES];
        gint plane_size[GST_VIDEO_MAX_PLANES];
        guint phys_addr;
        OMXR_MC_VIDEO_DECODERESULTTYPE *decode_res =
               (OMXR_MC_VIDEO_DECODERESULTTYPE *) omx_buf->
               omx_buf->pOutputPortPrivate;
        gint page_size;

        GST_DEBUG_OBJECT (pool, "Create dmabuf mem pBuffer=%p",
            omx_buf->omx_buf->pBuffer);

        vmeta = gst_buffer_get_video_meta (buf);

        phys_addr = (guint) decode_res->pvPhysImageAddressY;
        page_size = getpagesize ();

        /* Export a dmabuf file descriptor from the head of Y plane to
         * the end of the buffer so that mapping the whole plane as
         * contiguous memory is available. */
        if (!gst_omx_buffer_pool_export_dmabuf (pool, phys_addr,
                pool->port->port_def.nBufferSize, page_size,
                &vdbuf_data->id_export[0], &dmabuf_fd[0])) {
          GST_ERROR_OBJECT (pool, "dmabuf exporting failed");
          return GST_FLOW_ERROR;
        }

        plane_size[0] = vmeta->stride[0] *
            GST_VIDEO_INFO_COMP_HEIGHT (&pool->video_info, 0);

        /* Export dmabuf file descriptors from second and subsequent planes */
        n_planes = GST_VIDEO_INFO_N_PLANES (&pool->video_info);
        for (i = 1; i < n_planes; i++) {
          phys_addr = (guint) decode_res->pvPhysImageAddressY + vmeta->offset[i];
          plane_size[i] = vmeta->stride[i] *
              GST_VIDEO_INFO_COMP_HEIGHT (&pool->video_info, i);

          if (!gst_omx_buffer_pool_export_dmabuf (pool, phys_addr, plane_size[i],
                  page_size, &vdbuf_data->id_export[i], &dmabuf_fd[i])) {
            GST_ERROR_OBJECT (pool, "dmabuf exporting failed");
            return GST_FLOW_ERROR;
          }
        }

        if (pool->vsink_buf_req_supported)
          new_buf = gst_omx_buffer_pool_request_videosink_buffer_creation (pool,
              dmabuf_fd, vmeta->stride);
        else {
          GstVideoMeta *new_meta;

          new_buf = gst_buffer_new ();
          for (i = 0; i < n_planes; i++)
            gst_buffer_append_memory (new_buf,
              gst_dmabuf_allocator_alloc (pool->allocator, dmabuf_fd[i],
                  plane_size[i]));

          gst_buffer_add_video_meta_full (new_buf, GST_VIDEO_FRAME_FLAG_NONE,
              GST_VIDEO_INFO_FORMAT (&pool->video_info),
              GST_VIDEO_INFO_WIDTH (&pool->video_info),
              GST_VIDEO_INFO_HEIGHT (&pool->video_info),
              GST_VIDEO_INFO_N_PLANES (&pool->video_info), vmeta->offset,
              vmeta->stride);

          new_meta = gst_buffer_get_video_meta (new_buf);
          /* To avoid detaching meta data when a buffer returns
             to the buffer pool */
          GST_META_FLAG_SET (new_meta, GST_META_FLAG_POOLED);
        }

        g_ptr_array_remove_index (pool->buffers, pool->current_buffer_index);

        gst_mini_object_set_qdata (GST_MINI_OBJECT_CAST (buf),
            gst_omx_buffer_data_quark, NULL, NULL);

        gst_buffer_unref (buf);

        gst_mini_object_set_qdata (GST_MINI_OBJECT_CAST (new_buf),
            gst_omx_buffer_data_quark, omx_buf, NULL);

        g_ptr_array_add (pool->buffers, new_buf);

        *buffer = new_buf;
      } else
        *buffer = buf;
    } else
#endif
      *buffer = buf;

    vdbuf_data->already_acquired = TRUE;

    ret = GST_FLOW_OK;
  } else {
    /* Acquire any buffer that is available to be filled by upstream */
    ret =
        GST_BUFFER_POOL_CLASS (gst_omx_buffer_pool_parent_class)->acquire_buffer
        (bpool, buffer, params);
  }

  return ret;
}

static void
gst_omx_buffer_pool_release_buffer (GstBufferPool * bpool, GstBuffer * buffer)
{
  GstOMXBufferPool *pool = GST_OMX_BUFFER_POOL (bpool);
  OMX_ERRORTYPE err;
  GstOMXBuffer *omx_buf;

  g_assert (pool->component && pool->port);

  if (pool->allocating && !pool->deactivated) {
    GstOMXVideoDecBufferData *vdbuf_data;

    omx_buf =
        gst_mini_object_get_qdata (GST_MINI_OBJECT_CAST (buffer),
        gst_omx_buffer_data_quark);

    vdbuf_data = (GstOMXVideoDecBufferData *) omx_buf->private_data;

    if (pool->port->port_def.eDir == OMX_DirOutput && !omx_buf->used &&
        vdbuf_data->already_acquired) {
      /* Release back to the port, can be filled again */
      err = gst_omx_port_release_buffer (pool->port, omx_buf);
      if (err != OMX_ErrorNone) {
        GST_ELEMENT_ERROR (pool->element, LIBRARY, SETTINGS, (NULL),
            ("Failed to relase output buffer to component: %s (0x%08x)",
                gst_omx_error_to_string (err), err));
      }
      vdbuf_data->already_acquired = FALSE;
    } else if (pool->port->port_def.eDir == OMX_DirInput && !omx_buf->used) {
      /* TODO: Implement.
       *
       * If not used (i.e. was not passed to the component) this should do
       * the same as EmptyBufferDone.
       * If it is used (i.e. was passed to the component) this should do
       * nothing until EmptyBufferDone.
       *
       * EmptyBufferDone should release the buffer to the pool so it can
       * be allocated again
       *
       * Needs something to call back here in EmptyBufferDone, like keeping
       * a ref on the buffer in GstOMXBuffer until EmptyBufferDone... which
       * would ensure that the buffer is always unused when this is called.
       */
      g_assert_not_reached ();
      GST_BUFFER_POOL_CLASS (gst_omx_buffer_pool_parent_class)->release_buffer
          (bpool, buffer);
    }
  }
}

static void
gst_omx_buffer_pool_finalize (GObject * object)
{
  GstOMXBufferPool *pool = GST_OMX_BUFFER_POOL (object);

  if (pool->element)
    gst_object_unref (pool->element);
  pool->element = NULL;

  if (pool->buffers)
    g_ptr_array_unref (pool->buffers);
  pool->buffers = NULL;

  if (pool->other_pool)
    gst_object_unref (pool->other_pool);
  pool->other_pool = NULL;

  if (pool->allocator)
    gst_object_unref (pool->allocator);
  pool->allocator = NULL;

  if (pool->caps)
    gst_caps_unref (pool->caps);
  pool->caps = NULL;

  G_OBJECT_CLASS (gst_omx_buffer_pool_parent_class)->finalize (object);
}

static void
gst_omx_buffer_pool_class_init (GstOMXBufferPoolClass * klass)
{
  GObjectClass *gobject_class = (GObjectClass *) klass;
  GstBufferPoolClass *gstbufferpool_class = (GstBufferPoolClass *) klass;

  gst_omx_buffer_data_quark = g_quark_from_static_string ("GstOMXBufferData");

  gobject_class->finalize = gst_omx_buffer_pool_finalize;
  gstbufferpool_class->start = gst_omx_buffer_pool_start;
  gstbufferpool_class->stop = gst_omx_buffer_pool_stop;
  gstbufferpool_class->get_options = gst_omx_buffer_pool_get_options;
  gstbufferpool_class->set_config = gst_omx_buffer_pool_set_config;
  gstbufferpool_class->alloc_buffer = gst_omx_buffer_pool_alloc_buffer;
  gstbufferpool_class->free_buffer = gst_omx_buffer_pool_free_buffer;
  gstbufferpool_class->acquire_buffer = gst_omx_buffer_pool_acquire_buffer;
  gstbufferpool_class->release_buffer = gst_omx_buffer_pool_release_buffer;
}

static void
gst_omx_buffer_pool_init (GstOMXBufferPool * pool)
{
  pool->buffers = g_ptr_array_new ();
#ifdef HAVE_MMNGRBUF
  pool->allocator = gst_dmabuf_allocator_new ();
#else
  pool->allocator = g_object_new (gst_omx_memory_allocator_get_type (), NULL);
#endif
}

static GstBufferPool *
gst_omx_buffer_pool_new (GstElement * element, GstOMXComponent * component,
    GstOMXPort * port)
{
  GstOMXBufferPool *pool;

  pool = g_object_new (gst_omx_buffer_pool_get_type (), NULL);
  pool->element = gst_object_ref (element);
  pool->component = component;
  pool->port = port;
  pool->vsink_buf_req_supported = FALSE;

  return GST_BUFFER_POOL (pool);
}

typedef struct _BufferIdentification BufferIdentification;
struct _BufferIdentification
{
  guint64 timestamp;
};

static void
buffer_identification_free (BufferIdentification * id)
{
  g_slice_free (BufferIdentification, id);
}

/* prototypes */
static void gst_omx_video_dec_finalize (GObject * object);

static GstStateChangeReturn
gst_omx_video_dec_change_state (GstElement * element,
    GstStateChange transition);

static gboolean gst_omx_video_dec_open (GstVideoDecoder * decoder);
static gboolean gst_omx_video_dec_close (GstVideoDecoder * decoder);
static gboolean gst_omx_video_dec_start (GstVideoDecoder * decoder);
static gboolean gst_omx_video_dec_stop (GstVideoDecoder * decoder);
static gboolean gst_omx_video_dec_set_format (GstVideoDecoder * decoder,
    GstVideoCodecState * state);
static gboolean gst_omx_video_dec_flush (GstVideoDecoder * decoder,
    gboolean hard);
static GstFlowReturn gst_omx_video_dec_handle_frame (GstVideoDecoder * decoder,
    GstVideoCodecFrame * frame);
static GstFlowReturn gst_omx_video_dec_finish (GstVideoDecoder * decoder);
static gboolean gst_omx_video_dec_decide_allocation (GstVideoDecoder * bdec,
    GstQuery * query);
static gboolean gst_omx_video_dec_negotiate2 (GstVideoDecoder * decoder);

static GstFlowReturn gst_omx_video_dec_drain (GstOMXVideoDec * self,
    gboolean is_eos);

static OMX_ERRORTYPE gst_omx_video_dec_allocate_output_buffers (GstOMXVideoDec *
    self);
static OMX_ERRORTYPE gst_omx_video_dec_deallocate_output_buffers (GstOMXVideoDec
    * self);
static void gst_omx_video_dec_set_property (GObject * object, guint prop_id,
    const GValue * value, GParamSpec * pspec);
static void gst_omx_video_dec_get_property (GObject * object, guint prop_id,
    GValue * value, GParamSpec * pspec);
static void GstOMXBufCallbackfunc (struct GstOMXBufferCallback *);

enum
{
  PROP_0,
  PROP_NO_COPY,
  PROP_USE_DMABUF,
  PROP_NO_REORDER
};

/* class initialization */

#define DEBUG_INIT \
  GST_DEBUG_CATEGORY_INIT (gst_omx_video_dec_debug_category, "omxvideodec", 0, \
      "debug category for gst-omx video decoder base class");


G_DEFINE_ABSTRACT_TYPE_WITH_CODE (GstOMXVideoDec, gst_omx_video_dec,
    GST_TYPE_VIDEO_DECODER, DEBUG_INIT);

static gsize
gst_omx_video_dec_copy_frame (GstOMXVideoDec * self, GstBuffer * inbuf,
    guint offset, GstOMXBuffer * outbuf)
{
  guint size;

  size = gst_buffer_get_size (inbuf);

  /* Copy the buffer content in chunks of size as requested
   * by the port */
  outbuf->omx_buf->nFilledLen =
      MIN (size - offset,
      outbuf->omx_buf->nAllocLen - outbuf->omx_buf->nOffset);
  gst_buffer_extract (inbuf, offset,
      outbuf->omx_buf->pBuffer + outbuf->omx_buf->nOffset,
      outbuf->omx_buf->nFilledLen);

  return outbuf->omx_buf->nFilledLen;
}

static void
gst_omx_video_dec_class_init (GstOMXVideoDecClass * klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
  GstElementClass *element_class = GST_ELEMENT_CLASS (klass);
  GstVideoDecoderClass *video_decoder_class = GST_VIDEO_DECODER_CLASS (klass);

  gobject_class->finalize = gst_omx_video_dec_finalize;
  gobject_class->set_property = gst_omx_video_dec_set_property;
  gobject_class->get_property = gst_omx_video_dec_get_property;

  element_class->change_state =
      GST_DEBUG_FUNCPTR (gst_omx_video_dec_change_state);

  video_decoder_class->open = GST_DEBUG_FUNCPTR (gst_omx_video_dec_open);
  video_decoder_class->close = GST_DEBUG_FUNCPTR (gst_omx_video_dec_close);
  video_decoder_class->start = GST_DEBUG_FUNCPTR (gst_omx_video_dec_start);
  video_decoder_class->stop = GST_DEBUG_FUNCPTR (gst_omx_video_dec_stop);
  video_decoder_class->flush = GST_DEBUG_FUNCPTR (gst_omx_video_dec_flush);
  video_decoder_class->set_format =
      GST_DEBUG_FUNCPTR (gst_omx_video_dec_set_format);
  video_decoder_class->handle_frame =
      GST_DEBUG_FUNCPTR (gst_omx_video_dec_handle_frame);
  video_decoder_class->finish = GST_DEBUG_FUNCPTR (gst_omx_video_dec_finish);
  video_decoder_class->decide_allocation =
      GST_DEBUG_FUNCPTR (gst_omx_video_dec_decide_allocation);
  video_decoder_class->negotiate =
      GST_DEBUG_FUNCPTR (gst_omx_video_dec_negotiate2);

  klass->cdata.default_src_template_caps = "video/x-raw, "
      "width = " GST_VIDEO_SIZE_RANGE ", "
      "height = " GST_VIDEO_SIZE_RANGE ", " "framerate = " GST_VIDEO_FPS_RANGE;
  g_object_class_install_property (gobject_class, PROP_NO_COPY,
      g_param_spec_boolean ("no-copy", "No copy",
        "Whether or not to transfer decoded data without copy",
        FALSE, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS |
        GST_PARAM_MUTABLE_READY));
  g_object_class_install_property (gobject_class, PROP_USE_DMABUF,
      g_param_spec_boolean ("use-dmabuf", "Use dmabuffer ",
        "Whether or not to transfer decoded data using dmabuf",
        TRUE, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS |
        GST_PARAM_MUTABLE_READY));
  g_object_class_install_property (gobject_class, PROP_NO_REORDER,
        g_param_spec_boolean ("no-reorder", "Use video frame without reordering",
          "Whether or not to use video frame reordering",
          FALSE, G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS |
           GST_PARAM_MUTABLE_READY));

  klass->copy_frame = gst_omx_video_dec_copy_frame;
}

static void
gst_omx_video_dec_init (GstOMXVideoDec * self)
{
  gst_video_decoder_set_packetized (GST_VIDEO_DECODER (self), TRUE);

  g_mutex_init (&self->drain_lock);
  g_cond_init (&self->drain_cond);
  self->no_copy = FALSE;
#ifdef HAVE_MMNGRBUF
  self->use_dmabuf = TRUE;
#endif
  self->no_reorder = FALSE;
}

static gboolean
gst_omx_video_dec_open (GstVideoDecoder * decoder)
{
  GstOMXVideoDec *self = GST_OMX_VIDEO_DEC (decoder);
  GstOMXVideoDecClass *klass = GST_OMX_VIDEO_DEC_GET_CLASS (self);
  gint in_port_index, out_port_index;

  GST_DEBUG_OBJECT (self, "Opening decoder");

  self->dec =
      gst_omx_component_new (GST_OBJECT_CAST (self), klass->cdata.core_name,
      klass->cdata.component_name, klass->cdata.component_role,
      klass->cdata.hacks);
  self->started = FALSE;
  self->set_format_done = FALSE;

  if (!self->dec)
    return FALSE;

  if (gst_omx_component_get_state (self->dec,
          GST_CLOCK_TIME_NONE) != OMX_StateLoaded)
    return FALSE;

  in_port_index = klass->cdata.in_port_index;
  out_port_index = klass->cdata.out_port_index;

  if (in_port_index == -1 || out_port_index == -1) {
    OMX_PORT_PARAM_TYPE param;
    OMX_ERRORTYPE err;

    GST_OMX_INIT_STRUCT (&param);

    err =
        gst_omx_component_get_parameter (self->dec, OMX_IndexParamVideoInit,
        &param);
    if (err != OMX_ErrorNone) {
      GST_WARNING_OBJECT (self, "Couldn't get port information: %s (0x%08x)",
          gst_omx_error_to_string (err), err);
      /* Fallback */
      in_port_index = 0;
      out_port_index = 1;
    } else {
      GST_DEBUG_OBJECT (self, "Detected %u ports, starting at %u", param.nPorts,
          param.nStartPortNumber);
      in_port_index = param.nStartPortNumber + 0;
      out_port_index = param.nStartPortNumber + 1;
    }
  }
  self->dec_in_port = gst_omx_component_add_port (self->dec, in_port_index);
  self->dec_out_port = gst_omx_component_add_port (self->dec, out_port_index);

  if (!self->dec_in_port || !self->dec_out_port)
    return FALSE;

  GST_DEBUG_OBJECT (self, "Opened decoder");

  return TRUE;
}

static gboolean
gst_omx_video_dec_shutdown (GstOMXVideoDec * self)
{
  OMX_STATETYPE state;

  GST_DEBUG_OBJECT (self, "Shutting down decoder");

  state = gst_omx_component_get_state (self->dec, 0);
  if (state > OMX_StateLoaded || state == OMX_StateInvalid) {
    if (state > OMX_StateIdle) {
      gst_omx_component_set_state (self->dec, OMX_StateIdle);
      gst_omx_component_get_state (self->dec, 5 * GST_SECOND);
    }
    gst_omx_component_set_state (self->dec, OMX_StateLoaded);
    gst_omx_port_deallocate_buffers (self->dec_in_port);
    gst_omx_video_dec_deallocate_output_buffers (self);
    if (state > OMX_StateLoaded)
      gst_omx_component_get_state (self->dec, 5 * GST_SECOND);
  }

  return TRUE;
}

static gboolean
gst_omx_video_dec_close (GstVideoDecoder * decoder)
{
  GstOMXVideoDec *self = GST_OMX_VIDEO_DEC (decoder);

  GST_DEBUG_OBJECT (self, "Closing decoder");

  if (!gst_omx_video_dec_shutdown (self))
    return FALSE;

  self->dec_in_port = NULL;
  self->dec_out_port = NULL;
  if (self->dec)
    gst_omx_component_free (self->dec);
  self->dec = NULL;

  self->started = FALSE;
  self->set_format_done = FALSE;

  GST_DEBUG_OBJECT (self, "Closed decoder");

  return TRUE;
}

static void
gst_omx_video_dec_finalize (GObject * object)
{
  GstOMXVideoDec *self = GST_OMX_VIDEO_DEC (object);

  g_mutex_clear (&self->drain_lock);
  g_cond_clear (&self->drain_cond);

  G_OBJECT_CLASS (gst_omx_video_dec_parent_class)->finalize (object);
}

static GstStateChangeReturn
gst_omx_video_dec_change_state (GstElement * element, GstStateChange transition)
{
  GstOMXVideoDec *self;
  GstStateChangeReturn ret = GST_STATE_CHANGE_SUCCESS;

  g_return_val_if_fail (GST_IS_OMX_VIDEO_DEC (element),
      GST_STATE_CHANGE_FAILURE);
  self = GST_OMX_VIDEO_DEC (element);

  switch (transition) {
    case GST_STATE_CHANGE_NULL_TO_READY:
      break;
    case GST_STATE_CHANGE_READY_TO_PAUSED:
      self->downstream_flow_ret = GST_FLOW_OK;
      self->draining = FALSE;
      self->started = FALSE;
      break;
    case GST_STATE_CHANGE_PAUSED_TO_PLAYING:
      break;
    case GST_STATE_CHANGE_PAUSED_TO_READY:
      if (self->dec_in_port)
        gst_omx_port_set_flushing (self->dec_in_port, 5 * GST_SECOND, TRUE);
      if (self->dec_out_port)
        gst_omx_port_set_flushing (self->dec_out_port, 5 * GST_SECOND, TRUE);

      g_mutex_lock (&self->drain_lock);
      self->draining = FALSE;
      g_cond_broadcast (&self->drain_cond);
      g_mutex_unlock (&self->drain_lock);
      break;
    default:
      break;
  }

  if (ret == GST_STATE_CHANGE_FAILURE)
    return ret;

  ret =
      GST_ELEMENT_CLASS (gst_omx_video_dec_parent_class)->change_state
      (element, transition);

  if (ret == GST_STATE_CHANGE_FAILURE)
    return ret;

  switch (transition) {
    case GST_STATE_CHANGE_PLAYING_TO_PAUSED:
      break;
    case GST_STATE_CHANGE_PAUSED_TO_READY:
      self->downstream_flow_ret = GST_FLOW_FLUSHING;
      self->started = FALSE;

      if (!gst_omx_video_dec_shutdown (self))
        ret = GST_STATE_CHANGE_FAILURE;
      break;
    case GST_STATE_CHANGE_READY_TO_NULL:
      break;
    default:
      break;
  }

  return ret;
}

#define MAX_FRAME_DIST_TICKS  (5 * OMX_TICKS_PER_SECOND)
#define MAX_FRAME_DIST_FRAMES (100)

static GstVideoCodecFrame *
_find_nearest_frame (GstOMXVideoDec * self, GstOMXBuffer * buf)
{
  GList *l, *best_l = NULL;
  GList *finish_frames = NULL;
  GstVideoCodecFrame *best = NULL;
  guint64 best_timestamp = 0;
  guint64 best_diff = G_MAXUINT64;
  BufferIdentification *best_id = NULL;
  GList *frames;

  frames = gst_video_decoder_get_frames (GST_VIDEO_DECODER (self));

  for (l = frames; l; l = l->next) {
    GstVideoCodecFrame *tmp = l->data;
    BufferIdentification *id = gst_video_codec_frame_get_user_data (tmp);
    guint64 timestamp, diff;

    /* This happens for frames that were just added but
     * which were not passed to the component yet. Ignore
     * them here!
     */
    if (!id)
      continue;

    timestamp = id->timestamp;

    if (timestamp > buf->omx_buf->nTimeStamp)
      diff = timestamp - buf->omx_buf->nTimeStamp;
    else
      diff = buf->omx_buf->nTimeStamp - timestamp;

    if (best == NULL || diff < best_diff) {
      best = tmp;
      best_timestamp = timestamp;
      best_diff = diff;
      best_l = l;
      best_id = id;

      /* For frames without timestamp we simply take the first frame */
      if ((buf->omx_buf->nTimeStamp == 0 && timestamp == 0) || diff == 0)
        break;
    }
  }

  if (best_id) {
    for (l = frames; l && l != best_l; l = l->next) {
      GstVideoCodecFrame *tmp = l->data;
      BufferIdentification *id = gst_video_codec_frame_get_user_data (tmp);
      guint64 diff_ticks, diff_frames;

      /* This happens for frames that were just added but
       * which were not passed to the component yet. Ignore
       * them here!
       */
      if (!id)
        continue;

      if (id->timestamp > best_timestamp)
        break;

      if (id->timestamp == 0 || best_timestamp == 0)
        diff_ticks = 0;
      else
        diff_ticks = best_timestamp - id->timestamp;
      diff_frames = best->system_frame_number - tmp->system_frame_number;

      if (diff_ticks > MAX_FRAME_DIST_TICKS
          || diff_frames > MAX_FRAME_DIST_FRAMES) {
        finish_frames =
            g_list_prepend (finish_frames, gst_video_codec_frame_ref (tmp));
      }
    }
  }


  if (best)
    gst_video_codec_frame_ref (best);

  g_list_foreach (frames, (GFunc) gst_video_codec_frame_unref, NULL);
  g_list_free (frames);

  return best;
}

static gboolean
gst_omx_video_dec_fill_buffer (GstOMXVideoDec * self,
    GstOMXBuffer * inbuf, GstBuffer * outbuf)
{
  GstVideoCodecState *state =
      gst_video_decoder_get_output_state (GST_VIDEO_DECODER (self));
  GstVideoInfo *vinfo = &state->info;
  OMX_PARAM_PORTDEFINITIONTYPE *port_def = &self->dec_out_port->port_def;
  gboolean ret = FALSE;
  GstVideoFrame frame;

  if (vinfo->width != port_def->format.video.nFrameWidth ||
      vinfo->height != port_def->format.video.nFrameHeight) {
    GST_ERROR_OBJECT (self, "Resolution do not match. port: %dx%d vinfo: %dx%d",
        port_def->format.video.nFrameWidth, port_def->format.video.nFrameHeight,
        vinfo->width, vinfo->height);
    goto done;
  }

  /* Different strides */

  switch (vinfo->finfo->format) {
    case GST_VIDEO_FORMAT_I420:{
      gint i, j, height, width;
      guint8 *src, *dest;
      gint src_stride, dest_stride;

      gst_video_frame_map (&frame, vinfo, outbuf, GST_MAP_WRITE);
      for (i = 0; i < 3; i++) {
        if (i == 0) {
          src_stride = port_def->format.video.nStride;
          dest_stride = GST_VIDEO_FRAME_COMP_STRIDE (&frame, i);

          /* XXX: Try this if no stride was set */
          if (src_stride == 0)
            src_stride = dest_stride;
        } else {
          src_stride = port_def->format.video.nStride / 2;
          dest_stride = GST_VIDEO_FRAME_COMP_STRIDE (&frame, i);

          /* XXX: Try this if no stride was set */
          if (src_stride == 0)
            src_stride = dest_stride;
        }

        src = inbuf->omx_buf->pBuffer + inbuf->omx_buf->nOffset;
        if (i > 0)
          src +=
              port_def->format.video.nSliceHeight *
              port_def->format.video.nStride;
        if (i == 2)
          src +=
              (port_def->format.video.nSliceHeight / 2) *
              (port_def->format.video.nStride / 2);

        dest = GST_VIDEO_FRAME_COMP_DATA (&frame, i);
        height = GST_VIDEO_FRAME_COMP_HEIGHT (&frame, i);
        width = GST_VIDEO_FRAME_COMP_WIDTH (&frame, i);

        for (j = 0; j < height; j++) {
          memcpy (dest, src, width);
          src += src_stride;
          dest += dest_stride;
        }
      }
      gst_video_frame_unmap (&frame);
      ret = TRUE;
      break;
    }
    case GST_VIDEO_FORMAT_NV12:{
      gint i, j, height, width;
      guint8 *src, *dest;
      gint src_stride, dest_stride;

      gst_video_frame_map (&frame, vinfo, outbuf, GST_MAP_WRITE);
      for (i = 0; i < 2; i++) {
        if (i == 0) {
          src_stride = port_def->format.video.nStride;
          dest_stride = GST_VIDEO_FRAME_COMP_STRIDE (&frame, i);

          /* XXX: Try this if no stride was set */
          if (src_stride == 0)
            src_stride = dest_stride;
        } else {
          src_stride = port_def->format.video.nStride;
          dest_stride = GST_VIDEO_FRAME_COMP_STRIDE (&frame, i);

          /* XXX: Try this if no stride was set */
          if (src_stride == 0)
            src_stride = dest_stride;
        }

        src = inbuf->omx_buf->pBuffer + inbuf->omx_buf->nOffset;
        if (i == 1)
          src +=
              port_def->format.video.nSliceHeight *
              port_def->format.video.nStride;

        dest = GST_VIDEO_FRAME_COMP_DATA (&frame, i);
        height = GST_VIDEO_FRAME_COMP_HEIGHT (&frame, i);
        width = GST_VIDEO_FRAME_COMP_WIDTH (&frame, i) * (i == 0 ? 1 : 2);

        for (j = 0; j < height; j++) {
          memcpy (dest, src, width);
          src += src_stride;
          dest += dest_stride;
        }
      }
      gst_video_frame_unmap (&frame);
      ret = TRUE;
      break;
    }
    default:
      GST_ERROR_OBJECT (self, "Unsupported format");
      goto done;
      break;
  }


done:
  if (ret) {
    GST_BUFFER_PTS (outbuf) =
        gst_util_uint64_scale (inbuf->omx_buf->nTimeStamp, GST_SECOND,
        OMX_TICKS_PER_SECOND);
    if (inbuf->omx_buf->nTickCount != 0)
      GST_BUFFER_DURATION (outbuf) =
          gst_util_uint64_scale (inbuf->omx_buf->nTickCount, GST_SECOND,
          OMX_TICKS_PER_SECOND);
  }

  gst_video_codec_state_unref (state);

  return ret;
}

static OMX_ERRORTYPE
gst_omx_video_dec_allocate_output_buffers (GstOMXVideoDec * self)
{
  OMX_ERRORTYPE err = OMX_ErrorNone;
  GstOMXPort *port;
  GstBufferPool *pool;
  GstStructure *config;
  gboolean eglimage = FALSE, add_videometa = FALSE;
  GstCaps *caps = NULL;
  guint min = 0, max = 0;
  GstVideoCodecState *state =
      gst_video_decoder_get_output_state (GST_VIDEO_DECODER (self));

  port = self->dec_out_port;

  pool = gst_video_decoder_get_buffer_pool (GST_VIDEO_DECODER (self));
  /* FIXME: Enable this once there's a way to request downstream to
   * release all our buffers, e.g.
   * http://cgit.freedesktop.org/~wtay/gstreamer/log/?h=release-pool */
  if (FALSE && pool) {
    GstAllocator *allocator;

    config = gst_buffer_pool_get_config (pool);
    gst_buffer_pool_config_get_params (config, &caps, NULL, &min, &max);
    gst_buffer_pool_config_get_allocator (config, &allocator, NULL);

    /* Need at least 2 buffers for anything meaningful */
    min = MAX (MAX (min, port->port_def.nBufferCountMin), 4);
    if (max == 0) {
      max = min;
    } else if (max < port->port_def.nBufferCountMin || max < 2) {
      /* Can't use pool because can't have enough buffers */
      gst_caps_replace (&caps, NULL);
    } else {
      min = max;
    }

    add_videometa = gst_buffer_pool_config_has_option (config,
        GST_BUFFER_POOL_OPTION_VIDEO_META);

    /* TODO: Implement something here */
    eglimage = FALSE;
    caps = caps ? gst_caps_ref (caps) : NULL;

    GST_DEBUG_OBJECT (self, "Trying to use pool %p with caps %" GST_PTR_FORMAT
        " and memory type %s", pool, caps,
        (allocator ? allocator->mem_type : "(null)"));
  } else {
    gst_caps_replace (&caps, NULL);
    min = max = port->port_def.nBufferCountMin;
    GST_DEBUG_OBJECT (self, "No pool available, not negotiated yet");
  }

  if (caps)
    self->out_port_pool =
        gst_omx_buffer_pool_new (GST_ELEMENT_CAST (self), self->dec, port);

  /* TODO: Implement EGLImage handling and usage of other downstream buffers */

  /* If not using EGLImage or trying to use EGLImage failed */
  if (!eglimage) {
    gboolean was_enabled = TRUE;

    if (min != port->port_def.nBufferCountActual) {
      err = gst_omx_port_update_port_definition (port, NULL);
      if (err == OMX_ErrorNone) {
        port->port_def.nBufferCountActual = min;
        err = gst_omx_port_update_port_definition (port, &port->port_def);
      }

      if (err != OMX_ErrorNone) {
        GST_ERROR_OBJECT (self,
            "Failed to configure %u output buffers: %s (0x%08x)", min,
            gst_omx_error_to_string (err), err);
        goto done;
      }
    }

    if (!gst_omx_port_is_enabled (port)) {
      err = gst_omx_port_set_enabled (port, TRUE);
      if (err != OMX_ErrorNone) {
        GST_INFO_OBJECT (self,
            "Failed to enable port: %s (0x%08x)",
            gst_omx_error_to_string (err), err);
        goto done;
      }
      was_enabled = FALSE;
    }

    err = gst_omx_port_allocate_buffers (port);
    if (err != OMX_ErrorNone && min > port->port_def.nBufferCountMin) {
      GST_ERROR_OBJECT (self,
          "Failed to allocate required number of buffers %d, trying less and copying",
          min);
      min = port->port_def.nBufferCountMin;

      if (!was_enabled)
        gst_omx_port_set_enabled (port, FALSE);

      if (min != port->port_def.nBufferCountActual) {
        err = gst_omx_port_update_port_definition (port, NULL);
        if (err == OMX_ErrorNone) {
          port->port_def.nBufferCountActual = min;
          err = gst_omx_port_update_port_definition (port, &port->port_def);
        }

        if (err != OMX_ErrorNone) {
          GST_ERROR_OBJECT (self,
              "Failed to configure %u output buffers: %s (0x%08x)", min,
              gst_omx_error_to_string (err), err);
          goto done;
        }
      }

      err = gst_omx_port_allocate_buffers (port);

      /* Can't provide buffers downstream in this case */
      gst_caps_replace (&caps, NULL);
    }

    if (err != OMX_ErrorNone) {
      GST_ERROR_OBJECT (self, "Failed to allocate %d buffers: %s (0x%08x)", min,
          gst_omx_error_to_string (err), err);
      goto done;
    }

    if (!was_enabled) {
      err = gst_omx_port_wait_enabled (port, 2 * GST_SECOND);
      if (err != OMX_ErrorNone) {
        GST_ERROR_OBJECT (self,
            "Failed to wait until port is enabled: %s (0x%08x)",
            gst_omx_error_to_string (err), err);
        goto done;
      }
    }


  }

  err = OMX_ErrorNone;

  if (caps) {
    config = gst_buffer_pool_get_config (self->out_port_pool);

    if (add_videometa)
      gst_buffer_pool_config_add_option (config,
          GST_BUFFER_POOL_OPTION_VIDEO_META);

    gst_buffer_pool_config_set_params (config, caps,
        self->dec_out_port->port_def.nBufferSize, min, max);

    if (!gst_buffer_pool_set_config (self->out_port_pool, config)) {
      GST_INFO_OBJECT (self, "Failed to set config on internal pool");
      gst_object_unref (self->out_port_pool);
      self->out_port_pool = NULL;
      goto done;
    }

    GST_OMX_BUFFER_POOL (self->out_port_pool)->allocating = TRUE;
    /* This now allocates all the buffers */
    if (!gst_buffer_pool_set_active (self->out_port_pool, TRUE)) {
      GST_INFO_OBJECT (self, "Failed to activate internal pool");
      gst_object_unref (self->out_port_pool);
      self->out_port_pool = NULL;
    } else {
      GST_OMX_BUFFER_POOL (self->out_port_pool)->allocating = FALSE;
    }
  } else if (self->out_port_pool) {
    gst_object_unref (self->out_port_pool);
    self->out_port_pool = NULL;
  }

done:
  if (!self->out_port_pool && err == OMX_ErrorNone)
    GST_DEBUG_OBJECT (self,
        "Not using our internal pool and copying buffers for downstream");

  if (caps)
    gst_caps_unref (caps);
  if (pool)
    gst_object_unref (pool);
  if (state)
    gst_video_codec_state_unref (state);

  return err;
}

static OMX_ERRORTYPE
gst_omx_video_dec_deallocate_output_buffers (GstOMXVideoDec * self)
{
  OMX_ERRORTYPE err;

  if (self->out_port_pool) {
    gst_buffer_pool_set_active (self->out_port_pool, FALSE);
    GST_OMX_BUFFER_POOL (self->out_port_pool)->deactivated = TRUE;
    gst_object_unref (self->out_port_pool);
    self->out_port_pool = NULL;
  }
  err = gst_omx_port_deallocate_buffers (self->dec_out_port);

  return err;
}

static void GstOMXBufCallbackfunc (struct GstOMXBufferCallback *release)
{
  gint i;

  if (!release)
    return;

  if (release->buf != NULL) {
    gst_omx_port_release_buffer (release->out_port, release->buf);
  }

  g_free (release);
}

static GstBuffer *
gst_omx_video_dec_create_buffer_from_omx_output (GstOMXVideoDec * self,
    GstOMXBuffer * buf)
{
  /* Create a Gst buffer to wrap decoded data, then send to
   * downstream plugin.
   */
  GstBuffer *newbuf;
  GstVideoCodecState *state;
  GstVideoInfo *vinfo;
  gint i;
  gint offs, plane_size, used_size;
  gint width, base_stride, sliceheigh, height;
  OMX_PARAM_PORTDEFINITIONTYPE *port_def;
  GstMemory *mem;
  gsize offset[GST_VIDEO_MAX_PLANES];
  gint stride[GST_VIDEO_MAX_PLANES];


  state =
    gst_video_decoder_get_output_state (GST_VIDEO_DECODER (self));
  vinfo = &state->info;

  port_def    = &self->dec_out_port->port_def;
  width       = port_def->format.video.nFrameWidth;
  base_stride = port_def->format.video.nStride;
  sliceheigh  = port_def->format.video.nSliceHeight;
  height       = port_def->format.video.nFrameHeight;


  newbuf = gst_buffer_new ();

  /* Calculate memory area to add to Gst buffer */
  offs = 0;
  for (i=0; i < GST_VIDEO_INFO_N_PLANES(vinfo); i++) {
    offset[i] = offs;

    switch (GST_VIDEO_INFO_FORMAT(vinfo)) {
      case GST_VIDEO_FORMAT_NV12:
      case GST_VIDEO_FORMAT_NV21:
      case GST_VIDEO_FORMAT_NV16:
      case GST_VIDEO_FORMAT_NV24:
        /* The scale_width value is wrong for plane 2 of these
         * Semiplana formats. Need to multiply with 2 */
        stride[i] = (i == 0 ? 1 : 2) *
            GST_VIDEO_FORMAT_INFO_SCALE_WIDTH (vinfo->finfo, i,
            base_stride);
        break;
      default:
        stride[i] =
            GST_VIDEO_FORMAT_INFO_SCALE_WIDTH (vinfo->finfo, i,
            base_stride);
        break;
    }

    plane_size = stride[i] *
        GST_VIDEO_FORMAT_INFO_SCALE_HEIGHT (vinfo->finfo, i, sliceheigh);
    used_size = stride[i] *
        GST_VIDEO_FORMAT_INFO_SCALE_HEIGHT (vinfo->finfo, i, height);

    if (i == 0) {
      struct GstOMXBufferCallback *release;
      release = g_malloc (sizeof(struct GstOMXBufferCallback));
      release->out_port = self->dec_out_port;
      release->buf = buf;
      /* Add callback function to release OMX buffer to first plane */
      mem = gst_memory_new_wrapped (GST_MEMORY_FLAG_READONLY,
          buf->omx_buf->pBuffer + buf->omx_buf->nOffset + offs,
          plane_size, 0, used_size, release, GstOMXBufCallbackfunc);
    }
    else
      /* Only release OMX buffer one time. Do not add callback
       * function to other planes
       * (These planes are from same OMX buffer) */
      mem = gst_memory_new_wrapped (GST_MEMORY_FLAG_READONLY,
          buf->omx_buf->pBuffer + buf->omx_buf->nOffset + offs,
          plane_size, 0, used_size, NULL, NULL);

    gst_buffer_append_memory (newbuf, mem);

    offs += plane_size;
  }

  /* Add video meta data, which is needed to map frame */
  gst_buffer_add_video_meta_full (newbuf, GST_VIDEO_FRAME_FLAG_NONE,
      GST_VIDEO_INFO_FORMAT (vinfo), width, height,
      GST_VIDEO_INFO_N_PLANES(vinfo),
      offset, stride);

  /* Set timestamp */
  GST_BUFFER_PTS (newbuf) =
      gst_util_uint64_scale (buf->omx_buf->nTimeStamp, GST_SECOND,
      OMX_TICKS_PER_SECOND);
  if (buf->omx_buf->nTickCount != 0)
    GST_BUFFER_DURATION (newbuf) =
        gst_util_uint64_scale (buf->omx_buf->nTickCount, GST_SECOND,
        OMX_TICKS_PER_SECOND);

  return newbuf;
}

static void
gst_omx_video_dec_clean_older_frames (GstOMXVideoDec * self,
    GstOMXBuffer * buf, GList * frames)
{
  GList *l;
  GstClockTime timestamp;

  timestamp = gst_util_uint64_scale (buf->omx_buf->nTimeStamp, GST_SECOND,
      OMX_TICKS_PER_SECOND);

  if (GST_CLOCK_TIME_IS_VALID (timestamp)) {
    /* We could release all frames stored with pts < timestamp since the
     * decoder will likely output frames in display order */
    for (l = frames; l; l = l->next) {
      GstVideoCodecFrame *tmp = l->data;

      if (tmp->pts < timestamp) {
        gst_video_decoder_release_frame (GST_VIDEO_DECODER (self), tmp);
        GST_LOG_OBJECT (self,
            "discarding ghost frame %p (#%d) PTS:%" GST_TIME_FORMAT " DTS:%"
            GST_TIME_FORMAT, tmp, tmp->system_frame_number,
            GST_TIME_ARGS (tmp->pts), GST_TIME_ARGS (tmp->dts));
      } else {
        gst_video_codec_frame_unref (tmp);
      }
    }
  } else {
    /* We will release all frames with invalid timestamp because we don't even
     * know if they will be output some day. */
    for (l = frames; l; l = l->next) {
      GstVideoCodecFrame *tmp = l->data;

      if (!GST_CLOCK_TIME_IS_VALID (tmp->pts)) {
        gst_video_decoder_release_frame (GST_VIDEO_DECODER (self), tmp);
        GST_LOG_OBJECT (self,
            "discarding frame %p (#%d) with invalid PTS:%" GST_TIME_FORMAT
            " DTS:%" GST_TIME_FORMAT, tmp, tmp->system_frame_number,
            GST_TIME_ARGS (tmp->pts), GST_TIME_ARGS (tmp->dts));
      } else {
        gst_video_codec_frame_unref (tmp);
      }
    }
  }

  g_list_free (frames);
}

static void
gst_omx_video_dec_loop (GstOMXVideoDec * self)
{
  GstOMXPort *port = self->dec_out_port;
  GstOMXBuffer *buf = NULL;
  GstVideoCodecFrame *frame;
  GstFlowReturn flow_ret = GST_FLOW_OK;
  GstOMXAcquireBufferReturn acq_return;
  GstClockTimeDiff deadline;
  OMX_ERRORTYPE err;
  GstOMXVideoDecClass *klass = GST_OMX_VIDEO_DEC_GET_CLASS (self);

  acq_return = gst_omx_port_acquire_buffer (port, &buf);
  if (acq_return == GST_OMX_ACQUIRE_BUFFER_ERROR) {
    goto component_error;
  } else if (acq_return == GST_OMX_ACQUIRE_BUFFER_FLUSHING) {
    goto flushing;
  } else if (acq_return == GST_OMX_ACQUIRE_BUFFER_EOS) {
    goto eos;
  }

  if (!gst_pad_has_current_caps (GST_VIDEO_DECODER_SRC_PAD (self)) ||
      acq_return == GST_OMX_ACQUIRE_BUFFER_RECONFIGURE) {
    GstVideoCodecState *state;
    OMX_PARAM_PORTDEFINITIONTYPE port_def;
    GstVideoFormat format;

    GST_DEBUG_OBJECT (self, "Port settings have changed, updating caps");

    /* Reallocate all buffers */
    if (acq_return == GST_OMX_ACQUIRE_BUFFER_RECONFIGURE
        && gst_omx_port_is_enabled (port)) {
      err = gst_omx_port_set_enabled (port, FALSE);
      if (err != OMX_ErrorNone)
        goto reconfigure_error;

      err = gst_omx_port_wait_buffers_released (port, 5 * GST_SECOND);
      if (err != OMX_ErrorNone)
        goto reconfigure_error;

      err = gst_omx_video_dec_deallocate_output_buffers (self);
      if (err != OMX_ErrorNone)
        goto reconfigure_error;

      err = gst_omx_port_wait_enabled (port, 1 * GST_SECOND);
      if (err != OMX_ErrorNone)
        goto reconfigure_error;
    }

    GST_VIDEO_DECODER_STREAM_LOCK (self);

    gst_omx_port_get_port_definition (port, &port_def);
    g_assert (port_def.format.video.eCompressionFormat ==
        OMX_VIDEO_CodingUnused);

    switch (port_def.format.video.eColorFormat) {
      case OMX_COLOR_FormatYUV420Planar:
      case OMX_COLOR_FormatYUV420PackedPlanar:
        GST_DEBUG_OBJECT (self, "Output is I420 (%d)",
            port_def.format.video.eColorFormat);
        format = GST_VIDEO_FORMAT_I420;
        break;
      case OMX_COLOR_FormatYUV420SemiPlanar:
        GST_DEBUG_OBJECT (self, "Output is NV12 (%d)",
            port_def.format.video.eColorFormat);
        format = GST_VIDEO_FORMAT_NV12;
        break;
      default:
        GST_ERROR_OBJECT (self, "Unsupported color format: %d",
            port_def.format.video.eColorFormat);
        if (buf)
          gst_omx_port_release_buffer (self->dec_out_port, buf);
        GST_VIDEO_DECODER_STREAM_UNLOCK (self);
        goto caps_failed;
        break;
    }

    GST_DEBUG_OBJECT (self,
        "Setting output state: format %s, width %d, height %d",
        gst_video_format_to_string (format),
        port_def.format.video.nFrameWidth, port_def.format.video.nFrameHeight);

    state = gst_video_decoder_set_output_state (GST_VIDEO_DECODER (self),
        format, port_def.format.video.nFrameWidth,
        port_def.format.video.nFrameHeight, self->input_state);

    gst_omx_port_update_port_definition (self->dec_out_port, NULL);

    /* Take framerate and pixel-aspect-ratio from sinkpad caps */

    if (klass->cdata.hacks & GST_OMX_HACK_DEFAULT_PIXEL_ASPECT_RATIO) {
      /* Workaround in case video sink plugin only supports
       * default pixel-aspect-ratio 1/1   */
      state->info.par_d = state->info.par_n;
    }

    if (!gst_video_decoder_negotiate (GST_VIDEO_DECODER (self))) {
      if (buf)
        gst_omx_port_release_buffer (self->dec_out_port, buf);
      gst_video_codec_state_unref (state);
      goto caps_failed;
    }

    gst_video_codec_state_unref (state);

    GST_VIDEO_DECODER_STREAM_UNLOCK (self);

    if (acq_return == GST_OMX_ACQUIRE_BUFFER_RECONFIGURE) {
      err = gst_omx_video_dec_allocate_output_buffers (self);
      if (err != OMX_ErrorNone)
        goto reconfigure_error;

      err = gst_omx_port_populate (port);
      if (err != OMX_ErrorNone)
        goto reconfigure_error;

      err = gst_omx_port_mark_reconfigured (port);
      if (err != OMX_ErrorNone)
        goto reconfigure_error;
    }

    /* Now get a buffer */
    if (acq_return != GST_OMX_ACQUIRE_BUFFER_OK) {
      return;
    }
  }

  g_assert (acq_return == GST_OMX_ACQUIRE_BUFFER_OK);

  /* This prevents a deadlock between the srcpad stream
   * lock and the videocodec stream lock, if ::flush()
   * is called at the wrong time
   */
  if (gst_omx_port_is_flushing (self->dec_out_port)) {
    GST_DEBUG_OBJECT (self, "Flushing");
    gst_omx_port_release_buffer (self->dec_out_port, buf);
    goto flushing;
  }

  GST_DEBUG_OBJECT (self, "Handling buffer: 0x%08x %lu",
      buf->omx_buf->nFlags, buf->omx_buf->nTimeStamp);

  GST_VIDEO_DECODER_STREAM_LOCK (self);
  frame = _find_nearest_frame (self, buf);

  /* So we have a timestamped OMX buffer and get, or not, corresponding frame.
   * Assuming decoder output frames in display order, frames preceding this
   * frame could be discarded as they seems useless due to e.g interlaced
   * stream, corrupted input data...
   * In any cases, not likely to be seen again. so drop it before they pile up
   * and use all the memory. */
  if (self->no_reorder == FALSE)
    /* Only clean older frames in reorder mode. Do not clean in
     * no_reorder mode, as in that mode the output frames are not in
     * display order */
    gst_omx_video_dec_clean_older_frames (self, buf,
        gst_video_decoder_get_frames (GST_VIDEO_DECODER (self)));

  if (frame
      && (deadline = gst_video_decoder_get_max_decode_time
          (GST_VIDEO_DECODER (self), frame)) < 0) {
    GST_WARNING_OBJECT (self,
        "Frame is too late, dropping (deadline %" GST_TIME_FORMAT ")",
        GST_TIME_ARGS (-deadline));
    flow_ret = gst_video_decoder_drop_frame (GST_VIDEO_DECODER (self), frame);
    frame = NULL;
  } else if (frame &&
      !GST_VIDEO_CODEC_FRAME_IS_SYNC_POINT (frame) &&
      GST_VIDEO_DECODER (self)->output_segment.rate < 0.0) {
    GST_LOG_OBJECT (self,
        "Drop a frame which is not a keyframe in the backward playback");
    flow_ret = gst_video_decoder_drop_frame (GST_VIDEO_DECODER (self), frame);
    frame = NULL;
  } else if (!frame && buf->omx_buf->nFilledLen > 0) {
    GstBuffer *outbuf;

    /* This sometimes happens at EOS or if the input is not properly framed,
     * let's handle it gracefully by allocating a new buffer for the current
     * caps and filling it
     */

    GST_ERROR_OBJECT (self, "No corresponding frame found");

    if (self->out_port_pool) {
      gint i, n;
      GstBufferPoolAcquireParams params = { 0, };

      n = port->buffers->len;
      for (i = 0; i < n; i++) {
        GstBuffer *outbuf;
        GstOMXBuffer *tmp;

        outbuf =
            g_ptr_array_index (GST_OMX_BUFFER_POOL (self->
                out_port_pool)->buffers, i);
        tmp =
            gst_mini_object_get_qdata (GST_MINI_OBJECT_CAST (outbuf),
            gst_omx_buffer_data_quark);

        if (tmp == buf)
          break;
      }
      g_assert (i != n);

      GST_OMX_BUFFER_POOL (self->out_port_pool)->current_buffer_index = i;
      flow_ret =
          gst_buffer_pool_acquire_buffer (self->out_port_pool, &outbuf,
          &params);
      if (flow_ret != GST_FLOW_OK) {
        gst_omx_port_release_buffer (port, buf);
        goto invalid_buffer;
      }
      buf = NULL;
    } else {
      outbuf =
          gst_video_decoder_allocate_output_buffer (GST_VIDEO_DECODER (self));
      if (!gst_omx_video_dec_fill_buffer (self, buf, outbuf)) {
        gst_buffer_unref (outbuf);
        gst_omx_port_release_buffer (port, buf);
        goto invalid_buffer;
      }
    }

    flow_ret = gst_pad_push (GST_VIDEO_DECODER_SRC_PAD (self), outbuf);
  } else if (buf->omx_buf->nFilledLen > 0) {
    if (self->out_port_pool) {
      gint i, n;
      GstBufferPoolAcquireParams params = { 0, };

      n = port->buffers->len;
      for (i = 0; i < n; i++) {
        GstBuffer *outbuf;
        GstOMXBuffer *tmp;

        outbuf =
            g_ptr_array_index (GST_OMX_BUFFER_POOL (self->
                out_port_pool)->buffers, i);
        tmp =
            gst_mini_object_get_qdata (GST_MINI_OBJECT_CAST (outbuf),
            gst_omx_buffer_data_quark);

        if (tmp == buf)
          break;
      }
      g_assert (i != n);

      GST_OMX_BUFFER_POOL (self->out_port_pool)->current_buffer_index = i;
      flow_ret =
          gst_buffer_pool_acquire_buffer (self->out_port_pool,
          &frame->output_buffer, &params);
      if (flow_ret != GST_FLOW_OK) {
        flow_ret =
            gst_video_decoder_drop_frame (GST_VIDEO_DECODER (self), frame);
        frame = NULL;
        gst_omx_port_release_buffer (port, buf);
        goto invalid_buffer;
      }
      flow_ret =
          gst_video_decoder_finish_frame (GST_VIDEO_DECODER (self), frame);
      frame = NULL;
      buf = NULL;
    } else {
      if (self->no_copy)
      {
        /*
         * Replace output buffer from the bufferpool of the downstream plugin
         * with the one created with
         * gst_omx_video_dec_create_buffer_from_omx_output(), which sets each
         * plane address of an OMX output buffer to a new GstBuffer in order to
         * pass output image data to the downstream plugin without memcpy.
         */
        frame->output_buffer =
          gst_omx_video_dec_create_buffer_from_omx_output (self, buf);
        if (!frame->output_buffer) {
          GST_ERROR_OBJECT (self, "failed to create an output buffer");
          flow_ret = GST_FLOW_ERROR;
          goto flow_error;
        }
        gst_buffer_ref (frame->output_buffer);
        flow_ret =
            gst_video_decoder_finish_frame (GST_VIDEO_DECODER (self), frame);
        gst_buffer_unref (frame->output_buffer);
        frame = NULL;
        buf = NULL;
      } else {
        if ((flow_ret =
              gst_video_decoder_allocate_output_frame (GST_VIDEO_DECODER
                  (self), frame)) == GST_FLOW_OK) {
          /* FIXME: This currently happens because of a race condition too.
           * We first need to reconfigure the output port and then the input
           * port if both need reconfiguration.
           */

          if (!gst_omx_video_dec_fill_buffer (self, buf, frame->output_buffer)) {
            gst_buffer_replace (&frame->output_buffer, NULL);
            flow_ret =
              gst_video_decoder_drop_frame (GST_VIDEO_DECODER (self), frame);
            frame = NULL;
            gst_omx_port_release_buffer (port, buf);
            goto invalid_buffer;
          }
        }
        flow_ret =
            gst_video_decoder_finish_frame (GST_VIDEO_DECODER (self), frame);
        frame = NULL;
      }
    }
  } else if (frame != NULL) {
    flow_ret = gst_video_decoder_drop_frame (GST_VIDEO_DECODER (self), frame);
    frame = NULL;
  }

  GST_DEBUG_OBJECT (self, "Read frame from component");

  GST_DEBUG_OBJECT (self, "Finished frame: %s", gst_flow_get_name (flow_ret));

  if (buf) {
    err = gst_omx_port_release_buffer (port, buf);
    if (err != OMX_ErrorNone)
      goto release_error;
  }

  self->downstream_flow_ret = flow_ret;

  if (flow_ret != GST_FLOW_OK)
    goto flow_error;

  GST_VIDEO_DECODER_STREAM_UNLOCK (self);

  return;

component_error:
  {
    GST_ELEMENT_ERROR (self, LIBRARY, FAILED, (NULL),
        ("OpenMAX component in error state %s (0x%08x)",
            gst_omx_component_get_last_error_string (self->dec),
            gst_omx_component_get_last_error (self->dec)));
    gst_pad_push_event (GST_VIDEO_DECODER_SRC_PAD (self), gst_event_new_eos ());
    gst_pad_pause_task (GST_VIDEO_DECODER_SRC_PAD (self));
    self->downstream_flow_ret = GST_FLOW_ERROR;
    self->started = FALSE;
    return;
  }

flushing:
  {
    GST_DEBUG_OBJECT (self, "Flushing -- stopping task");
    gst_pad_pause_task (GST_VIDEO_DECODER_SRC_PAD (self));
    self->downstream_flow_ret = GST_FLOW_FLUSHING;
    self->started = FALSE;
    return;
  }

eos:
  {
    g_mutex_lock (&self->drain_lock);
    if (self->draining) {
      GST_DEBUG_OBJECT (self, "Drained");
      self->draining = FALSE;
      g_cond_broadcast (&self->drain_cond);
      flow_ret = GST_FLOW_OK;
      gst_pad_pause_task (GST_VIDEO_DECODER_SRC_PAD (self));
    } else {
      GST_DEBUG_OBJECT (self, "Component signalled EOS");
      flow_ret = GST_FLOW_EOS;
    }
    g_mutex_unlock (&self->drain_lock);
    self->downstream_flow_ret = flow_ret;

    /* Here we fallback and pause the task for the EOS case */
    if (flow_ret != GST_FLOW_OK)
      goto flow_error;

    GST_VIDEO_DECODER_STREAM_UNLOCK (self);

    return;
  }

flow_error:
  {
    if (flow_ret == GST_FLOW_EOS) {
      GST_DEBUG_OBJECT (self, "EOS");

      gst_pad_push_event (GST_VIDEO_DECODER_SRC_PAD (self),
          gst_event_new_eos ());
      gst_pad_pause_task (GST_VIDEO_DECODER_SRC_PAD (self));
    } else if (flow_ret == GST_FLOW_NOT_LINKED || flow_ret < GST_FLOW_EOS) {
      GST_ELEMENT_ERROR (self, STREAM, FAILED,
          ("Internal data stream error."), ("stream stopped, reason %s",
              gst_flow_get_name (flow_ret)));

      gst_pad_push_event (GST_VIDEO_DECODER_SRC_PAD (self),
          gst_event_new_eos ());
      gst_pad_pause_task (GST_VIDEO_DECODER_SRC_PAD (self));
    }
    self->started = FALSE;
    GST_VIDEO_DECODER_STREAM_UNLOCK (self);
    return;
  }

reconfigure_error:
  {
    GST_ELEMENT_ERROR (self, LIBRARY, SETTINGS, (NULL),
        ("Unable to reconfigure output port"));
    gst_pad_push_event (GST_VIDEO_DECODER_SRC_PAD (self), gst_event_new_eos ());
    gst_pad_pause_task (GST_VIDEO_DECODER_SRC_PAD (self));
    self->downstream_flow_ret = GST_FLOW_ERROR;
    self->started = FALSE;
    return;
  }

invalid_buffer:
  {
    GST_ELEMENT_ERROR (self, LIBRARY, SETTINGS, (NULL),
        ("Invalid sized input buffer"));
    gst_pad_push_event (GST_VIDEO_DECODER_SRC_PAD (self), gst_event_new_eos ());
    gst_pad_pause_task (GST_VIDEO_DECODER_SRC_PAD (self));
    self->downstream_flow_ret = GST_FLOW_NOT_NEGOTIATED;
    self->started = FALSE;
    GST_VIDEO_DECODER_STREAM_UNLOCK (self);
    return;
  }

caps_failed:
  {
    GST_ELEMENT_ERROR (self, LIBRARY, SETTINGS, (NULL), ("Failed to set caps"));
    gst_pad_push_event (GST_VIDEO_DECODER_SRC_PAD (self), gst_event_new_eos ());
    gst_pad_pause_task (GST_VIDEO_DECODER_SRC_PAD (self));
    GST_VIDEO_DECODER_STREAM_UNLOCK (self);
    self->downstream_flow_ret = GST_FLOW_NOT_NEGOTIATED;
    self->started = FALSE;
    return;
  }
release_error:
  {
    GST_ELEMENT_ERROR (self, LIBRARY, SETTINGS, (NULL),
        ("Failed to relase output buffer to component: %s (0x%08x)",
            gst_omx_error_to_string (err), err));
    gst_pad_push_event (GST_VIDEO_DECODER_SRC_PAD (self), gst_event_new_eos ());
    gst_pad_pause_task (GST_VIDEO_DECODER_SRC_PAD (self));
    self->downstream_flow_ret = GST_FLOW_ERROR;
    self->started = FALSE;
    GST_VIDEO_DECODER_STREAM_UNLOCK (self);
    return;
  }
}

static gboolean
gst_omx_video_dec_start (GstVideoDecoder * decoder)
{
  GstOMXVideoDec *self;

  self = GST_OMX_VIDEO_DEC (decoder);

  self->last_upstream_ts = 0;
  self->eos = FALSE;
  self->downstream_flow_ret = GST_FLOW_OK;

  return TRUE;
}

static gboolean
gst_omx_video_dec_stop (GstVideoDecoder * decoder)
{
  GstOMXVideoDec *self;

  self = GST_OMX_VIDEO_DEC (decoder);

  GST_DEBUG_OBJECT (self, "Stopping decoder");

  gst_omx_port_set_flushing (self->dec_in_port, 5 * GST_SECOND, TRUE);
  gst_omx_port_set_flushing (self->dec_out_port, 5 * GST_SECOND, TRUE);

  gst_pad_stop_task (GST_VIDEO_DECODER_SRC_PAD (decoder));

  if (gst_omx_component_get_state (self->dec, 0) > OMX_StateIdle)
    gst_omx_component_set_state (self->dec, OMX_StateIdle);

  self->downstream_flow_ret = GST_FLOW_FLUSHING;
  self->started = FALSE;
  self->eos = FALSE;

  g_mutex_lock (&self->drain_lock);
  self->draining = FALSE;
  g_cond_broadcast (&self->drain_cond);
  g_mutex_unlock (&self->drain_lock);

  gst_omx_component_get_state (self->dec, 5 * GST_SECOND);

  gst_buffer_replace (&self->codec_data, NULL);

  if (self->input_state)
    gst_video_codec_state_unref (self->input_state);
  self->input_state = NULL;

  GST_DEBUG_OBJECT (self, "Stopped decoder");

  return TRUE;
}

typedef struct
{
  GstVideoFormat format;
  OMX_COLOR_FORMATTYPE type;
} VideoNegotiationMap;

static void
video_negotiation_map_free (VideoNegotiationMap * m)
{
  g_slice_free (VideoNegotiationMap, m);
}

static GList *
gst_omx_video_dec_get_supported_colorformats (GstOMXVideoDec * self)
{
  GstOMXPort *port = self->dec_out_port;
  OMX_VIDEO_PARAM_PORTFORMATTYPE param;
  OMX_ERRORTYPE err;
  GList *negotiation_map = NULL;
  gint i;
  OMX_COLOR_FORMATTYPE format_org;
  VideoNegotiationMap *m;
  const VideoNegotiationMap format_list[] = {
    {GST_VIDEO_FORMAT_NV12, OMX_COLOR_FormatYUV420SemiPlanar},
    {GST_VIDEO_FORMAT_I420, OMX_COLOR_FormatYUV420Planar},
  };

  GST_OMX_INIT_STRUCT (&param);
  param.nPortIndex = port->index;

  err = gst_omx_component_get_parameter (self->dec,
      OMX_IndexParamVideoPortFormat, &param);
  if (err != OMX_ErrorNone) {
    GST_ERROR_OBJECT (self,
        "Failed to getting video port format (err info: %s (0x%08x))",
        gst_omx_error_to_string (err), err);
    return NULL;
  }
  /* temporary save original format type */
  format_org = param.eColorFormat;

  for (i = 0; i < sizeof (format_list) / sizeof (VideoNegotiationMap); i++) {
    param.eColorFormat = format_list[i].type;
    err = gst_omx_component_set_parameter (self->dec,
        OMX_IndexParamVideoPortFormat, &param);
    if (err == OMX_ErrorNone) {
      m = g_slice_new (VideoNegotiationMap);
      m->format = format_list[i].format;
      m->type = format_list[i].type;
      negotiation_map = g_list_append (negotiation_map, m);
      GST_DEBUG_OBJECT (self, "Component supports (%d)", param.eColorFormat);
    }
  }

  /* restore setting */
  param.eColorFormat = format_org;
  err = gst_omx_component_set_parameter (self->dec,
      OMX_IndexParamVideoPortFormat, &param);
  if (err != OMX_ErrorNone)
    GST_ERROR_OBJECT (self,
        "Failed to seetting video port format (err info: %s (0x%08x))",
        gst_omx_error_to_string (err), err);

  return negotiation_map;
}

static gboolean
gst_omx_video_dec_negotiate (GstOMXVideoDec * self)
{
  OMX_VIDEO_PARAM_PORTFORMATTYPE param;
  OMX_ERRORTYPE err;
  GstCaps *comp_supported_caps;
  GList *negotiation_map = NULL, *l;
  GstCaps *templ_caps, *intersection;
  GstVideoFormat format;
  GstStructure *s;
  const gchar *format_str;

  GST_DEBUG_OBJECT (self, "Trying to negotiate a video format with downstream");

  templ_caps = gst_pad_get_pad_template_caps (GST_VIDEO_DECODER_SRC_PAD (self));
  intersection =
      gst_pad_peer_query_caps (GST_VIDEO_DECODER_SRC_PAD (self), templ_caps);
  gst_caps_unref (templ_caps);

  GST_DEBUG_OBJECT (self, "Allowed downstream caps: %" GST_PTR_FORMAT,
      intersection);

  negotiation_map = gst_omx_video_dec_get_supported_colorformats (self);
  comp_supported_caps = gst_caps_new_empty ();
  for (l = negotiation_map; l; l = l->next) {
    VideoNegotiationMap *map = l->data;

    gst_caps_append_structure (comp_supported_caps,
        gst_structure_new ("video/x-raw",
            "format", G_TYPE_STRING,
            gst_video_format_to_string (map->format), NULL));
  }

  if (!gst_caps_is_empty (comp_supported_caps)) {
    GstCaps *tmp;

    tmp = gst_caps_intersect (comp_supported_caps, intersection);
    gst_caps_unref (intersection);
    intersection = tmp;
  }
  gst_caps_unref (comp_supported_caps);

  if (gst_caps_is_empty (intersection)) {
    gst_caps_unref (intersection);
    GST_ERROR_OBJECT (self, "Empty caps");
    g_list_free_full (negotiation_map,
        (GDestroyNotify) video_negotiation_map_free);
    return FALSE;
  }

  intersection = gst_caps_truncate (intersection);
  intersection = gst_caps_fixate (intersection);

  s = gst_caps_get_structure (intersection, 0);
  format_str = gst_structure_get_string (s, "format");
  if (!format_str ||
      (format =
          gst_video_format_from_string (format_str)) ==
      GST_VIDEO_FORMAT_UNKNOWN) {
    GST_ERROR_OBJECT (self, "Invalid caps: %" GST_PTR_FORMAT, intersection);
    g_list_free_full (negotiation_map,
        (GDestroyNotify) video_negotiation_map_free);
    return FALSE;
  }

  GST_OMX_INIT_STRUCT (&param);
  param.nPortIndex = self->dec_out_port->index;

  for (l = negotiation_map; l; l = l->next) {
    VideoNegotiationMap *m = l->data;

    if (m->format == format) {
      param.eColorFormat = m->type;
      break;
    }
  }

  GST_DEBUG_OBJECT (self, "Negotiating color format %s (%d)", format_str,
      param.eColorFormat);

  /* We must find something here */
  g_assert (l != NULL);
  g_list_free_full (negotiation_map,
      (GDestroyNotify) video_negotiation_map_free);

  err =
      gst_omx_component_set_parameter (self->dec,
      OMX_IndexParamVideoPortFormat, &param);
  if (err != OMX_ErrorNone) {
    GST_ERROR_OBJECT (self, "Failed to set video port format: %s (0x%08x)",
        gst_omx_error_to_string (err), err);
  }

  return (err == OMX_ErrorNone);
}

#ifdef ENABLE_NV12_PAGE_ALIGN
static gint
get_uv_offset_aligned_to_page (gint page_size, gint stride, gint height)
{
  gint a, b, r;
  gint lcm;

  /*
   * The following implementation uses the Euclidean Algorithm to obtain
   * the least common multiple of stride and page size.
   */

  /* nStride is set to width, to achieve 4K aligned by adjusting
     the nSliceHeight. */
  /* (1) Calculate the GCD of stride and alignment */
  b = stride;
  a = page_size;
  while ((r = a % b) != 0) {
    a = b;
    b = r;
  }

  /* (2) Calculate the LCM of stride and alignment */
  lcm = stride * page_size / b;

  /* (3) Calculate the offset of UV plane */
  return (((stride * height) / lcm) + 1) * lcm;
}

static gboolean
gst_omx_video_dec_align_uv_offset_to_page (GstOMXVideoDec * self,
    OMX_PARAM_PORTDEFINITIONTYPE * out_port_def, gint page_size, gint stride,
    gint height)
{
  gint uv_offset;

  uv_offset = get_uv_offset_aligned_to_page (page_size, stride, height);

  out_port_def->format.video.nStride = stride;
  out_port_def->format.video.nSliceHeight = uv_offset / stride;

  GST_DEBUG_OBJECT (self,
      "Set nSliceHeight to %u for aligning the UV plane offset to the page size",
      (guint) out_port_def->format.video.nSliceHeight);

  if (gst_omx_port_update_port_definition (self->dec_out_port,
          out_port_def) != OMX_ErrorNone)
    return FALSE;

  return TRUE;
}
#endif /* ENABLE_NV12_PAGE_ALIGN */

static gboolean
gst_omx_video_dec_set_format (GstVideoDecoder * decoder,
    GstVideoCodecState * state)
{
  GstOMXVideoDec *self;
  GstOMXVideoDecClass *klass;
  GstVideoInfo *info = &state->info;
  gboolean is_format_change = FALSE;
  gboolean needs_disable = FALSE;
  OMX_PARAM_PORTDEFINITIONTYPE port_def;
#ifdef ENABLE_NV12_PAGE_ALIGN
  OMX_PARAM_PORTDEFINITIONTYPE out_port_def;
  gint page_size;
#endif

  self = GST_OMX_VIDEO_DEC (decoder);
  klass = GST_OMX_VIDEO_DEC_GET_CLASS (decoder);

  GST_DEBUG_OBJECT (self, "Setting new caps %" GST_PTR_FORMAT, state->caps);

  gst_omx_port_get_port_definition (self->dec_in_port, &port_def);

  /* Check if the caps change is a real format change or if only irrelevant
   * parts of the caps have changed or nothing at all.
   */
  is_format_change |= port_def.format.video.nFrameWidth != info->width;
  is_format_change |= port_def.format.video.nFrameHeight != info->height;
  is_format_change |= (port_def.format.video.xFramerate == 0
      && info->fps_n != 0)
      || (port_def.format.video.xFramerate !=
      (info->fps_n << 16) / (info->fps_d));
  is_format_change |= (self->codec_data != state->codec_data);
  if (klass->is_format_change)
    is_format_change |=
        klass->is_format_change (self, self->dec_in_port, state);

  needs_disable =
      gst_omx_component_get_state (self->dec,
      GST_CLOCK_TIME_NONE) != OMX_StateLoaded;
  /* If the component is not in Loaded state and a real format change happens
   * we have to disable the port and re-allocate all buffers. If no real
   * format change happened we can just exit here.
   */
  if (needs_disable && !is_format_change) {
    GST_DEBUG_OBJECT (self,
        "Already running and caps did not change the format");
    if (self->input_state)
      gst_video_codec_state_unref (self->input_state);
    self->input_state = gst_video_codec_state_ref (state);
    return TRUE;
  }

  if (needs_disable && is_format_change) {
    GST_DEBUG_OBJECT (self, "Need to disable and drain decoder");

    gst_omx_video_dec_drain (self, FALSE);
    gst_omx_port_set_flushing (self->dec_out_port, 5 * GST_SECOND, TRUE);

    /* Wait until the srcpad loop is finished,
     * unlock GST_VIDEO_DECODER_STREAM_LOCK to prevent deadlocks
     * caused by using this lock from inside the loop function */
    GST_VIDEO_DECODER_STREAM_UNLOCK (self);
    gst_pad_stop_task (GST_VIDEO_DECODER_SRC_PAD (decoder));
    GST_VIDEO_DECODER_STREAM_LOCK (self);

    if (klass->cdata.hacks & GST_OMX_HACK_NO_COMPONENT_RECONFIGURE) {
      GST_VIDEO_DECODER_STREAM_UNLOCK (self);
      gst_omx_video_dec_stop (GST_VIDEO_DECODER (self));
      gst_omx_video_dec_close (GST_VIDEO_DECODER (self));
      GST_VIDEO_DECODER_STREAM_LOCK (self);

      if (!gst_omx_video_dec_open (GST_VIDEO_DECODER (self)))
        return FALSE;
      needs_disable = FALSE;
    } else {
      if (gst_omx_port_set_enabled (self->dec_in_port, FALSE) != OMX_ErrorNone)
        return FALSE;
      if (gst_omx_port_set_enabled (self->dec_out_port, FALSE) != OMX_ErrorNone)
        return FALSE;
      if (gst_omx_port_wait_buffers_released (self->dec_in_port,
              5 * GST_SECOND) != OMX_ErrorNone)
        return FALSE;
      if (gst_omx_port_wait_buffers_released (self->dec_out_port,
              1 * GST_SECOND) != OMX_ErrorNone)
        return FALSE;
      if (gst_omx_port_deallocate_buffers (self->dec_in_port) != OMX_ErrorNone)
        return FALSE;
      if (gst_omx_video_dec_deallocate_output_buffers (self) != OMX_ErrorNone)
        return FALSE;
      if (gst_omx_port_wait_enabled (self->dec_in_port,
              1 * GST_SECOND) != OMX_ErrorNone)
        return FALSE;
      if (gst_omx_port_wait_enabled (self->dec_out_port,
              1 * GST_SECOND) != OMX_ErrorNone)
        return FALSE;
    }
    if (self->input_state)
      gst_video_codec_state_unref (self->input_state);
    self->input_state = NULL;

    GST_DEBUG_OBJECT (self, "Decoder drained and disabled");
  }

  port_def.format.video.nFrameWidth = info->width;
  port_def.format.video.nFrameHeight = info->height;
  if (info->fps_n == 0)
    port_def.format.video.xFramerate = 0;
  else
    port_def.format.video.xFramerate = (info->fps_n << 16) / (info->fps_d);

  GST_DEBUG_OBJECT (self, "Setting inport port definition");

  if (gst_omx_port_update_port_definition (self->dec_in_port,
          &port_def) != OMX_ErrorNone)
    return FALSE;

  if (klass->set_format) {
    if (!klass->set_format (self, self->dec_in_port, state)) {
      GST_ERROR_OBJECT (self, "Subclass failed to set the new format");
      return FALSE;
    }
  }

  GST_DEBUG_OBJECT (self, "Updating outport port definition");
  if (gst_omx_port_update_port_definition (self->dec_out_port,
          NULL) != OMX_ErrorNone)
    return FALSE;

  gst_buffer_replace (&self->codec_data, state->codec_data);
  self->input_state = gst_video_codec_state_ref (state);

  GST_DEBUG_OBJECT (self, "Enabling component");

  if (needs_disable) {
    if (gst_omx_port_set_enabled (self->dec_in_port, TRUE) != OMX_ErrorNone)
      return FALSE;
    if (gst_omx_port_allocate_buffers (self->dec_in_port) != OMX_ErrorNone)
      return FALSE;
    if (gst_omx_port_wait_enabled (self->dec_in_port,
            5 * GST_SECOND) != OMX_ErrorNone)
      return FALSE;
    if (gst_omx_port_mark_reconfigured (self->dec_in_port) != OMX_ErrorNone)
      return FALSE;
  } else {
    if (!gst_omx_video_dec_negotiate (self))
      GST_LOG_OBJECT (self, "Negotiation failed, will get output format later");

#ifdef ENABLE_NV12_PAGE_ALIGN
    gst_omx_port_get_port_definition (self->dec_out_port, &out_port_def);

    page_size = getpagesize ();
    if (out_port_def.format.video.eColorFormat ==
        OMX_COLOR_FormatYUV420SemiPlanar &&
        (info->width * info->height) & (page_size - 1))
      if (!gst_omx_video_dec_align_uv_offset_to_page (self, &out_port_def,
              page_size, info->width, info->height)) {
        GST_ERROR_OBJECT (self,
            "Failed to align the uv offset of the NV12 plane to the page size");
        return FALSE;
      }
#endif

    if (gst_omx_component_set_state (self->dec, OMX_StateIdle) != OMX_ErrorNone)
      return FALSE;

    /* Need to allocate buffers to reach Idle state */
    if (gst_omx_port_allocate_buffers (self->dec_in_port) != OMX_ErrorNone)
      return FALSE;

    if (self->use_dmabuf)
      self->out_port_pool =
        gst_omx_buffer_pool_new (GST_ELEMENT_CAST (self), self->dec,
        self->dec_out_port);

    if (gst_omx_port_allocate_buffers (self->dec_out_port) != OMX_ErrorNone)
      return FALSE;

    if (gst_omx_component_get_state (self->dec,
            GST_CLOCK_TIME_NONE) != OMX_StateIdle)
      return FALSE;

    if (gst_omx_component_set_state (self->dec,
            OMX_StateExecuting) != OMX_ErrorNone)
      return FALSE;

    if (gst_omx_component_get_state (self->dec,
            GST_CLOCK_TIME_NONE) != OMX_StateExecuting)
      return FALSE;
  }

  /* Unset flushing to allow ports to accept data again */
  gst_omx_port_set_flushing (self->dec_in_port, 5 * GST_SECOND, FALSE);
  gst_omx_port_set_flushing (self->dec_out_port, 5 * GST_SECOND, FALSE);

  if (!needs_disable) {
    if (gst_omx_port_populate (self->dec_out_port) != OMX_ErrorNone)
      return FALSE;
  }

  if (gst_omx_component_get_last_error (self->dec) != OMX_ErrorNone) {
    GST_ERROR_OBJECT (self, "Component in error state: %s (0x%08x)",
        gst_omx_component_get_last_error_string (self->dec),
        gst_omx_component_get_last_error (self->dec));
    return FALSE;
  }

  /* Start the srcpad loop again */
  GST_DEBUG_OBJECT (self, "Starting task again");

  self->downstream_flow_ret = GST_FLOW_OK;
  gst_pad_start_task (GST_VIDEO_DECODER_SRC_PAD (self),
      (GstTaskFunction) gst_omx_video_dec_loop, decoder, NULL);
  self->set_format_done = TRUE;

  return TRUE;
}

static gboolean
gst_omx_video_dec_flush (GstVideoDecoder * decoder, gboolean hard)
{
  GstOMXVideoDec *self;

  self = GST_OMX_VIDEO_DEC (decoder);

  /* FIXME: Handle different values of hard */

  GST_DEBUG_OBJECT (self, "Flushing decoder");

  gst_omx_port_set_flushing (self->dec_in_port, 5 * GST_SECOND, TRUE);
  gst_omx_port_set_flushing (self->dec_out_port, 5 * GST_SECOND, TRUE);

  /* Wait until the srcpad loop is finished,
   * unlock GST_VIDEO_DECODER_STREAM_LOCK to prevent deadlocks
   * caused by using this lock from inside the loop function */
  GST_VIDEO_DECODER_STREAM_UNLOCK (self);
  GST_PAD_STREAM_LOCK (GST_VIDEO_DECODER_SRC_PAD (self));
  GST_PAD_STREAM_UNLOCK (GST_VIDEO_DECODER_SRC_PAD (self));
  GST_VIDEO_DECODER_STREAM_LOCK (self);

  gst_omx_port_set_flushing (self->dec_in_port, 5 * GST_SECOND, FALSE);
  gst_omx_port_set_flushing (self->dec_out_port, 5 * GST_SECOND, FALSE);
  gst_omx_port_populate (self->dec_out_port);

  /* Start the srcpad loop again */
  self->last_upstream_ts = 0;
  self->eos = FALSE;
  self->downstream_flow_ret = GST_FLOW_OK;
  if (self->set_format_done)
    gst_pad_start_task (GST_VIDEO_DECODER_SRC_PAD (self),
        (GstTaskFunction) gst_omx_video_dec_loop, decoder, NULL);

  GST_DEBUG_OBJECT (self, "Flush decoder");

  return TRUE;
}

static GstFlowReturn
gst_omx_video_dec_handle_frame (GstVideoDecoder * decoder,
    GstVideoCodecFrame * frame)
{
  GstOMXAcquireBufferReturn acq_ret = GST_OMX_ACQUIRE_BUFFER_ERROR;
  GstOMXVideoDec *self;
  GstOMXVideoDecClass *klass;
  GstOMXPort *port;
  GstOMXBuffer *buf;
  GstBuffer *codec_data = NULL;
  guint offset = 0, size;
  GstClockTime timestamp, duration;
  OMX_ERRORTYPE err;
  gsize inbuf_consumed;

  self = GST_OMX_VIDEO_DEC (decoder);
  klass = GST_OMX_VIDEO_DEC_GET_CLASS (self);

  self->ts_flag = FALSE;  /* reset this flag for each buffer */

  GST_DEBUG_OBJECT (self, "Handling frame");

  if (self->eos) {
    GST_WARNING_OBJECT (self, "Got frame after EOS");
    gst_video_codec_frame_unref (frame);
    return GST_FLOW_EOS;
  }

  if (!self->started && !GST_VIDEO_CODEC_FRAME_IS_SYNC_POINT (frame)) {
    gst_video_decoder_drop_frame (GST_VIDEO_DECODER (self), frame);
    return GST_FLOW_OK;
  }


  /* Workaround for timestamp issue */
  if (!GST_CLOCK_TIME_IS_VALID (frame->pts) &&
        GST_CLOCK_TIME_IS_VALID (frame->dts))
    frame->pts = frame->dts;

  timestamp = frame->pts;
  duration = frame->duration;

  if (self->downstream_flow_ret != GST_FLOW_OK) {
    gst_video_codec_frame_unref (frame);
    return self->downstream_flow_ret;
  }

  if (klass->prepare_frame) {
    GstFlowReturn ret;

    ret = klass->prepare_frame (self, frame);
    if (ret != GST_FLOW_OK) {
      GST_ERROR_OBJECT (self, "Preparing frame failed: %s",
          gst_flow_get_name (ret));
      gst_video_codec_frame_unref (frame);
      return ret;
    }
  }

  port = self->dec_in_port;

  size = gst_buffer_get_size (frame->input_buffer);
  while (offset < size) {
    /* Make sure to release the base class stream lock, otherwise
     * _loop() can't call _finish_frame() and we might block forever
     * because no input buffers are released */
    GST_VIDEO_DECODER_STREAM_UNLOCK (self);
    acq_ret = gst_omx_port_acquire_buffer (port, &buf);

    if (acq_ret == GST_OMX_ACQUIRE_BUFFER_ERROR) {
      GST_VIDEO_DECODER_STREAM_LOCK (self);
      goto component_error;
    } else if (acq_ret == GST_OMX_ACQUIRE_BUFFER_FLUSHING) {
      GST_VIDEO_DECODER_STREAM_LOCK (self);
      goto flushing;
    } else if (acq_ret == GST_OMX_ACQUIRE_BUFFER_RECONFIGURE) {
      /* Reallocate all buffers */
      err = gst_omx_port_set_enabled (port, FALSE);
      if (err != OMX_ErrorNone) {
        GST_VIDEO_DECODER_STREAM_LOCK (self);
        goto reconfigure_error;
      }

      err = gst_omx_port_wait_buffers_released (port, 5 * GST_SECOND);
      if (err != OMX_ErrorNone) {
        GST_VIDEO_DECODER_STREAM_LOCK (self);
        goto reconfigure_error;
      }

      err = gst_omx_port_deallocate_buffers (port);
      if (err != OMX_ErrorNone) {
        GST_VIDEO_DECODER_STREAM_LOCK (self);
        goto reconfigure_error;
      }

      err = gst_omx_port_wait_enabled (port, 1 * GST_SECOND);
      if (err != OMX_ErrorNone) {
        GST_VIDEO_DECODER_STREAM_LOCK (self);
        goto reconfigure_error;
      }

      err = gst_omx_port_set_enabled (port, TRUE);
      if (err != OMX_ErrorNone) {
        GST_VIDEO_DECODER_STREAM_LOCK (self);
        goto reconfigure_error;
      }

      err = gst_omx_port_allocate_buffers (port);
      if (err != OMX_ErrorNone) {
        GST_VIDEO_DECODER_STREAM_LOCK (self);
        goto reconfigure_error;
      }

      err = gst_omx_port_wait_enabled (port, 5 * GST_SECOND);
      if (err != OMX_ErrorNone) {
        GST_VIDEO_DECODER_STREAM_LOCK (self);
        goto reconfigure_error;
      }

      err = gst_omx_port_mark_reconfigured (port);
      if (err != OMX_ErrorNone) {
        GST_VIDEO_DECODER_STREAM_LOCK (self);
        goto reconfigure_error;
      }

      /* Now get a new buffer and fill it */
      GST_VIDEO_DECODER_STREAM_LOCK (self);
      continue;
    }
    GST_VIDEO_DECODER_STREAM_LOCK (self);

    g_assert (acq_ret == GST_OMX_ACQUIRE_BUFFER_OK && buf != NULL);

    if (buf->omx_buf->nAllocLen - buf->omx_buf->nOffset <= 0) {
      gst_omx_port_release_buffer (port, buf);
      goto full_buffer;
    }

    if (self->downstream_flow_ret != GST_FLOW_OK) {
      gst_omx_port_release_buffer (port, buf);
      goto flow_error;
    }

    if (self->codec_data) {
      GST_DEBUG_OBJECT (self, "Passing codec data to the component");

      codec_data = self->codec_data;

      if (buf->omx_buf->nAllocLen - buf->omx_buf->nOffset <
          gst_buffer_get_size (codec_data)) {
        gst_omx_port_release_buffer (port, buf);
        goto too_large_codec_data;
      }

      buf->omx_buf->nFlags |= OMX_BUFFERFLAG_CODECCONFIG;
      buf->omx_buf->nFilledLen = gst_buffer_get_size (codec_data);
      gst_buffer_extract (codec_data, 0,
          buf->omx_buf->pBuffer + buf->omx_buf->nOffset,
          buf->omx_buf->nFilledLen);

      if (GST_CLOCK_TIME_IS_VALID (timestamp))
        buf->omx_buf->nTimeStamp =
            gst_util_uint64_scale (timestamp, OMX_TICKS_PER_SECOND, GST_SECOND);
      else
        buf->omx_buf->nTimeStamp = 0;
      buf->omx_buf->nTickCount = 0;

      self->started = TRUE;
      err = gst_omx_port_release_buffer (port, buf);
      gst_buffer_replace (&self->codec_data, NULL);
      if (err != OMX_ErrorNone)
        goto release_error;
      /* Acquire new buffer for the actual frame */
      continue;
    }

    /* Now handle the frame */
    GST_DEBUG_OBJECT (self, "Passing frame offset %d to the component", offset);

    inbuf_consumed = klass->copy_frame (self, frame->input_buffer, offset, buf);
    if (inbuf_consumed < 0) {
      GST_ERROR_OBJECT (self, "Failed to copy an input frame");
      self->downstream_flow_ret = GST_FLOW_ERROR;
      goto flow_error;
    }

    if (timestamp != GST_CLOCK_TIME_NONE) {
      self->last_upstream_ts = timestamp;
    } else {
      /* Video stream does not provide timestamp, try calculate */
      /* Skip calculate if the buffer does not contains any meaningful
       * data (ts_flag = FALSE ) */
      if (offset == 0 && self->ts_flag) {
        if (duration != GST_CLOCK_TIME_NONE )
          /* In case timestamp is invalid. may use duration to calculate
           * timestamp */
          self->last_upstream_ts += duration;
        else
          /* Use default fps value as last resort */
          self->last_upstream_ts += gst_util_uint64_scale (1,
                GST_SECOND, DEFAULT_FRAME_PER_SECOND);

        timestamp = self->last_upstream_ts;
        frame->pts = timestamp;
      }
    }

    buf->omx_buf->nTimeStamp =
      gst_util_uint64_scale (timestamp, OMX_TICKS_PER_SECOND, GST_SECOND);

    buf->omx_buf->nTickCount =
          gst_util_uint64_scale (inbuf_consumed, duration, size);

    if (offset == 0) {
      BufferIdentification *id = g_slice_new0 (BufferIdentification);

      if (GST_VIDEO_CODEC_FRAME_IS_SYNC_POINT (frame))
        buf->omx_buf->nFlags |= OMX_BUFFERFLAG_SYNCFRAME;

      id->timestamp = buf->omx_buf->nTimeStamp;
      gst_video_codec_frame_set_user_data (frame, id,
          (GDestroyNotify) buffer_identification_free);
    }

    /* TODO: Set flags
     *   - OMX_BUFFERFLAG_DECODEONLY for buffers that are outside
     *     the segment
     */

    offset += inbuf_consumed;

    if (offset == size)
      buf->omx_buf->nFlags |= OMX_BUFFERFLAG_ENDOFFRAME;

    if (GST_BUFFER_FLAG_IS_SET (frame->input_buffer, GST_BUFFER_FLAG_HEADER))
      buf->omx_buf->nFlags |= OMX_BUFFERFLAG_CODECCONFIG;

    self->started = TRUE;
    err = gst_omx_port_release_buffer (port, buf);
    if (err != OMX_ErrorNone)
      goto release_error;
  }

  gst_video_codec_frame_unref (frame);

  GST_DEBUG_OBJECT (self, "Passed frame to component");

  return self->downstream_flow_ret;

full_buffer:
  {
    gst_video_codec_frame_unref (frame);
    GST_ELEMENT_ERROR (self, LIBRARY, FAILED, (NULL),
        ("Got OpenMAX buffer with no free space (%p, %u/%u)", buf,
            buf->omx_buf->nOffset, buf->omx_buf->nAllocLen));
    return GST_FLOW_ERROR;
  }

flow_error:
  {
    gst_video_codec_frame_unref (frame);

    return self->downstream_flow_ret;
  }

too_large_codec_data:
  {
    gst_video_codec_frame_unref (frame);
    GST_ELEMENT_ERROR (self, STREAM, FORMAT, (NULL),
        ("codec_data larger than supported by OpenMAX port (%u > %u)",
            gst_buffer_get_size (codec_data),
            self->dec_in_port->port_def.nBufferSize));
    return GST_FLOW_ERROR;
  }

component_error:
  {
    gst_video_codec_frame_unref (frame);
    GST_ELEMENT_ERROR (self, LIBRARY, FAILED, (NULL),
        ("OpenMAX component in error state %s (0x%08x)",
            gst_omx_component_get_last_error_string (self->dec),
            gst_omx_component_get_last_error (self->dec)));
    return GST_FLOW_ERROR;
  }

flushing:
  {
    gst_video_codec_frame_unref (frame);
    GST_DEBUG_OBJECT (self, "Flushing -- returning FLUSHING");
    return GST_FLOW_FLUSHING;
  }
reconfigure_error:
  {
    gst_video_codec_frame_unref (frame);
    GST_ELEMENT_ERROR (self, LIBRARY, SETTINGS, (NULL),
        ("Unable to reconfigure input port"));
    return GST_FLOW_ERROR;
  }
release_error:
  {
    gst_video_codec_frame_unref (frame);
    GST_ELEMENT_ERROR (self, LIBRARY, SETTINGS, (NULL),
        ("Failed to relase input buffer to component: %s (0x%08x)",
            gst_omx_error_to_string (err), err));
    return GST_FLOW_ERROR;
  }
}

static GstFlowReturn
gst_omx_video_dec_finish (GstVideoDecoder * decoder)
{
  GstOMXVideoDec *self;

  self = GST_OMX_VIDEO_DEC (decoder);

  return gst_omx_video_dec_drain (self, TRUE);
}

static GstFlowReturn
gst_omx_video_dec_drain (GstOMXVideoDec * self, gboolean is_eos)
{
  GstOMXVideoDecClass *klass;
  GstOMXBuffer *buf;
  GstOMXAcquireBufferReturn acq_ret;
  OMX_ERRORTYPE err;

  GST_DEBUG_OBJECT (self, "Draining component");

  klass = GST_OMX_VIDEO_DEC_GET_CLASS (self);

  if (!self->started) {
    GST_DEBUG_OBJECT (self, "Component not started yet");
    return GST_FLOW_OK;
  }
  self->started = FALSE;

  /* Don't send EOS buffer twice, this doesn't work */
  if (self->eos) {
    GST_DEBUG_OBJECT (self, "Component is EOS already");
    return GST_FLOW_OK;
  }
  if (is_eos)
    self->eos = TRUE;

  if ((klass->cdata.hacks & GST_OMX_HACK_NO_EMPTY_EOS_BUFFER)) {
    GST_WARNING_OBJECT (self, "Component does not support empty EOS buffers");
    return GST_FLOW_OK;
  }

  /* Make sure to release the base class stream lock, otherwise
   * _loop() can't call _finish_frame() and we might block forever
   * because no input buffers are released */
  GST_VIDEO_DECODER_STREAM_UNLOCK (self);

  /* Send an EOS buffer to the component and let the base
   * class drop the EOS event. We will send it later when
   * the EOS buffer arrives on the output port. */
  acq_ret = gst_omx_port_acquire_buffer (self->dec_in_port, &buf);
  if (acq_ret != GST_OMX_ACQUIRE_BUFFER_OK) {
    GST_VIDEO_DECODER_STREAM_LOCK (self);
    GST_ERROR_OBJECT (self, "Failed to acquire buffer for draining: %d",
        acq_ret);
    return GST_FLOW_ERROR;
  }

  g_mutex_lock (&self->drain_lock);
  self->draining = TRUE;
  buf->omx_buf->nFilledLen = 0;
  buf->omx_buf->nTimeStamp =
      gst_util_uint64_scale (self->last_upstream_ts, OMX_TICKS_PER_SECOND,
      GST_SECOND);
  buf->omx_buf->nTickCount = 0;
  buf->omx_buf->nFlags |= OMX_BUFFERFLAG_EOS;
  err = gst_omx_port_release_buffer (self->dec_in_port, buf);
  if (err != OMX_ErrorNone) {
    GST_ERROR_OBJECT (self, "Failed to drain component: %s (0x%08x)",
        gst_omx_error_to_string (err), err);
    GST_VIDEO_DECODER_STREAM_LOCK (self);
    return GST_FLOW_ERROR;
  }

  GST_DEBUG_OBJECT (self, "Waiting until component is drained");

  if (G_UNLIKELY (self->dec->hacks & GST_OMX_HACK_DRAIN_MAY_NOT_RETURN)) {
    gint64 wait_until = g_get_monotonic_time () + G_TIME_SPAN_SECOND / 2;

    if (!g_cond_wait_until (&self->drain_cond, &self->drain_lock, wait_until))
      GST_WARNING_OBJECT (self, "Drain timed out");
    else
      GST_DEBUG_OBJECT (self, "Drained component");

  } else {
    g_cond_wait (&self->drain_cond, &self->drain_lock);
    GST_DEBUG_OBJECT (self, "Drained component");
  }

  g_mutex_unlock (&self->drain_lock);
  GST_VIDEO_DECODER_STREAM_LOCK (self);

  self->started = FALSE;

  return GST_FLOW_OK;
}

static gboolean
gst_omx_video_dec_decide_allocation (GstVideoDecoder * bdec, GstQuery * query)
{
  GstBufferPool *pool, *vpool;
  GstStructure *config, *vconfig;
  GstOMXVideoDec *self;
  GstCaps *caps;
  gboolean update_pool = FALSE;

  self = GST_OMX_VIDEO_DEC (bdec);

  if (self->out_port_pool) {
    if (gst_query_get_n_allocation_pools (query) > 0) {
      gst_query_parse_nth_allocation_pool (query, 0, &pool, NULL, NULL, NULL);
      g_assert (pool != NULL);

      config = gst_buffer_pool_get_config (pool);
      gst_structure_get_boolean (config,
          "videosink_buffer_creation_request_supported",
          &GST_OMX_BUFFER_POOL (self->out_port_pool)->vsink_buf_req_supported);
      gst_object_unref (pool);
      update_pool = TRUE;
    }

    /* Set pool parameters to our own configuration */
    config = gst_buffer_pool_get_config (self->out_port_pool);

    gst_buffer_pool_config_add_option (config,
        GST_BUFFER_POOL_OPTION_VIDEO_META);

    gst_query_parse_allocation (query, &caps, NULL);
    gst_buffer_pool_config_set_params (config, caps,
        self->dec_out_port->port_def.nBufferSize,
        self->dec_out_port->port_def.nBufferCountActual,
        self->dec_out_port->port_def.nBufferCountActual);

    if (!gst_buffer_pool_set_config (self->out_port_pool, config)) {
      GST_ERROR_OBJECT (self, "Failed to set config on internal pool");
      gst_object_unref (self->out_port_pool);
      self->out_port_pool = NULL;
      return FALSE;
    }

    GST_OMX_BUFFER_POOL (self->out_port_pool)->allocating = TRUE;
    gst_buffer_pool_set_active (self->out_port_pool, TRUE);

    /* This video buffer pool created below will not be used, just setting to
     * the gstvideodecoder class through a query, because it is
     * mandatoty to set a buffer pool into the gstvideodecoder class
     * regardless of whether the buffer pool is actually used or not.
     * gst-omx controls its own buffer pool by itself, so the buffer pool
     * gst-omx will use does not have to be set to the gstvideodecoder
     * class.
     * When the gstbufferpool is activated, it allocates buffers from
     * a gstallocator for the number of min_buffers in advance, which is
     * the parameter of a buffer pool. No buffers will be allocated with
     * the video buffer pool created below even when being activated,
     * because the min_buffers is set as 0.
     */
    vpool = gst_video_buffer_pool_new ();
    vconfig = gst_buffer_pool_get_config (vpool);
    gst_buffer_pool_config_set_params (vconfig, caps, 0, 0, 1);
    gst_buffer_pool_set_config (vpool, vconfig);

    if (update_pool)
      gst_query_set_nth_allocation_pool (query, 0, vpool,
          self->dec_out_port->port_def.nBufferSize, 0, 1);
    else
      gst_query_add_allocation_pool (query, vpool,
          self->dec_out_port->port_def.nBufferSize, 0, 1);
    gst_object_unref (vpool);
  } else {
    if (!GST_VIDEO_DECODER_CLASS
        (gst_omx_video_dec_parent_class)->decide_allocation (bdec, query))
      return FALSE;

    g_assert (gst_query_get_n_allocation_pools (query) > 0);
    gst_query_parse_nth_allocation_pool (query, 0, &pool, NULL, NULL, NULL);
    g_assert (pool != NULL);

    config = gst_buffer_pool_get_config (pool);
    if (gst_query_find_allocation_meta (query, GST_VIDEO_META_API_TYPE, NULL)) {
      gst_buffer_pool_config_add_option (config,
          GST_BUFFER_POOL_OPTION_VIDEO_META);
    }
    gst_buffer_pool_set_config (pool, config);
    gst_object_unref (pool);
  }

  return TRUE;
}
static void
gst_omx_video_dec_set_property (GObject * object, guint prop_id,
    const GValue * value, GParamSpec * pspec)
{
  GstOMXVideoDec *self = GST_OMX_VIDEO_DEC (object);

  switch (prop_id) {
    case PROP_NO_COPY:
    {
      self->no_copy = g_value_get_boolean (value);
      self->use_dmabuf = FALSE;
      break;
    }
    case PROP_USE_DMABUF:
      self->use_dmabuf = g_value_get_boolean (value);
      break;
    case PROP_NO_REORDER:
      self->no_reorder = g_value_get_boolean (value);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}
static void
gst_omx_video_dec_get_property (GObject * object, guint prop_id,
    GValue * value, GParamSpec * pspec)
{
  GstOMXVideoDec *self = GST_OMX_VIDEO_DEC (object);

  switch (prop_id) {
    case PROP_NO_COPY:
      g_value_set_boolean (value, self->no_copy);
      break;
    case PROP_USE_DMABUF:
      g_value_set_boolean (value, self->use_dmabuf);
      break;
    case PROP_NO_REORDER:
      g_value_set_boolean (value, self->no_reorder);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static gboolean
gst_omx_video_dec_negotiate2 (GstVideoDecoder * decoder)
{
  GstOMXVideoDec *self;
  GstVideoCodecState *state;
  GstCaps *prevcaps;

  self = GST_OMX_VIDEO_DEC (decoder);

  state = gst_video_decoder_get_output_state (decoder);
  if (state == NULL) {
    GST_ERROR_OBJECT (self, "Failed to get output state");
    return FALSE;
  }

  if (state->caps == NULL)
    state->caps = gst_video_info_to_caps (&state->info);

  prevcaps = gst_pad_get_current_caps (decoder->srcpad);
  if (prevcaps && gst_caps_is_equal (prevcaps, state->caps)) {
    GST_DEBUG_OBJECT (self,
        "Skip the video decoder negotiation because the caps is not changed");
    gst_video_codec_state_unref (state);
    return TRUE;
  }

  gst_video_codec_state_unref (state);

  return GST_VIDEO_DECODER_CLASS
      (gst_omx_video_dec_parent_class)->negotiate (decoder);
}

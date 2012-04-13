/*
 * Copyright 2010, The Android Open Source Project
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *  * Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 *  * Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT OWNER OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 * OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#define LOG_TAG "GLWebViewState"
#define LOG_NDEBUG 1

#include "config.h"
#include "GLWebViewState.h"

#if USE(ACCELERATED_COMPOSITING)

#include "AndroidLog.h"
#include "BaseLayerAndroid.h"
#include "ClassTracker.h"
#include "GLUtils.h"
#include "ImagesManager.h"
#include "LayerAndroid.h"
#include "private/hwui/DrawGlInfo.h"
#include "ScrollableLayerAndroid.h"
#include "SkPath.h"
#include "TilesManager.h"
#include "TransferQueue.h"
#include "SurfaceCollection.h"
#include "SurfaceCollectionManager.h"
#include <pthread.h>
#include <wtf/CurrentTime.h>

// log warnings if scale goes outside this range
#define MIN_SCALE_WARNING 0.1
#define MAX_SCALE_WARNING 10

// fps indicator is FPS_INDICATOR_HEIGHT pixels high.
// The max width is equal to MAX_FPS_VALUE fps.
#define FPS_INDICATOR_HEIGHT 10
#define MAX_FPS_VALUE 60

#define COLLECTION_SWAPPED_COUNTER_MODULE 10

namespace WebCore {

using namespace android;

GLWebViewState::GLWebViewState()
    : m_frameworkLayersInval(0, 0, 0, 0)
    , m_isScrolling(false)
    , m_isViewportScrolling(false)
    , m_goingDown(true)
    , m_goingLeft(false)
    , m_scale(1)
    , m_layersRenderingMode(kAllTextures)
    , m_surfaceCollectionManager(this)
{
    m_viewport.setEmpty();

#ifdef DEBUG_COUNT
    ClassTracker::instance()->increment("GLWebViewState");
#endif
#ifdef MEASURES_PERF
    m_timeCounter = 0;
    m_totalTimeCounter = 0;
    m_measurePerfs = false;
#endif
}

GLWebViewState::~GLWebViewState()
{
#ifdef DEBUG_COUNT
    ClassTracker::instance()->decrement("GLWebViewState");
#endif

}

bool GLWebViewState::setBaseLayer(BaseLayerAndroid* layer, bool showVisualIndicator,
                                  bool isPictureAfterFirstLayout)
{
    if (!layer || isPictureAfterFirstLayout)
        m_layersRenderingMode = kAllTextures;

    SurfaceCollection* collection = 0;
    if (layer) {
        ALOGV("layer tree %p, with child %p", layer, layer->getChild(0));
        layer->setState(this);
        collection = new SurfaceCollection(layer);
    }
    bool queueFull = m_surfaceCollectionManager.updateWithSurfaceCollection(
        collection, isPictureAfterFirstLayout);
    m_glExtras.setDrawExtra(0);

#ifdef MEASURES_PERF
    if (m_measurePerfs && !showVisualIndicator)
        dumpMeasures();
    m_measurePerfs = showVisualIndicator;
#endif

    TilesManager::instance()->setShowVisualIndicator(showVisualIndicator);
    return queueFull;
}

void GLWebViewState::scrollLayer(int layerId, int x, int y)
{
    m_surfaceCollectionManager.updateScrollableLayer(layerId, x, y);
}

void GLWebViewState::setViewport(const SkRect& viewport, float scale)
{
    // allocate max possible number of tiles visible with this viewport / expandedTileBounds
    const float invTileContentWidth = scale / TilesManager::tileWidth();
    const float invTileContentHeight = scale / TilesManager::tileHeight();

    int viewMaxTileX = static_cast<int>(ceilf((viewport.width()-1) * invTileContentWidth)) + 1;
    int viewMaxTileY = static_cast<int>(ceilf((viewport.height()-1) * invTileContentHeight)) + 1;

    TilesManager* tilesManager = TilesManager::instance();
    int maxTextureCount = viewMaxTileX * viewMaxTileY * (tilesManager->highEndGfx() ? 4 : 2);

    tilesManager->setMaxTextureCount(maxTextureCount);

    // TODO: investigate whether we can move this return earlier.
    if ((m_viewport == viewport)
        && (m_scale == scale)) {
        // everything below will stay the same, early return.
        m_isViewportScrolling = false;
        return;
    }
    m_scale = scale;

    m_goingDown = m_viewport.fTop - viewport.fTop <= 0;
    m_goingLeft = m_viewport.fLeft - viewport.fLeft >= 0;

    // detect viewport scrolling from short programmatic scrolls/jumps
    m_isViewportScrolling = m_viewport != viewport && SkRect::Intersects(m_viewport, viewport);
    m_viewport = viewport;

    ALOGV("New VIEWPORT %.2f - %.2f %.2f - %.2f (w: %2.f h: %.2f scale: %.2f )",
          m_viewport.fLeft, m_viewport.fTop, m_viewport.fRight, m_viewport.fBottom,
          m_viewport.width(), m_viewport.height(), scale);
}

#ifdef MEASURES_PERF
void GLWebViewState::dumpMeasures()
{
    for (int i = 0; i < m_timeCounter; i++) {
        ALOGD("%d delay: %d ms", m_totalTimeCounter + i,
             static_cast<int>(m_delayTimes[i]*1000));
        m_delayTimes[i] = 0;
    }
    m_totalTimeCounter += m_timeCounter;
    m_timeCounter = 0;
}
#endif // MEASURES_PERF

void GLWebViewState::addDirtyArea(const IntRect& rect)
{
    if (rect.isEmpty())
        return;

    IntRect inflatedRect = rect;
    inflatedRect.inflate(8);
    if (m_frameworkLayersInval.isEmpty())
        m_frameworkLayersInval = inflatedRect;
    else
        m_frameworkLayersInval.unite(inflatedRect);
}

void GLWebViewState::resetLayersDirtyArea()
{
    m_frameworkLayersInval.setX(0);
    m_frameworkLayersInval.setY(0);
    m_frameworkLayersInval.setWidth(0);
    m_frameworkLayersInval.setHeight(0);
}

double GLWebViewState::setupDrawing(const IntRect& viewRect, const SkRect& visibleRect,
                                    const IntRect& webViewRect, int titleBarHeight,
                                    const IntRect& screenClip, float scale)
{
    int left = viewRect.x();
    int top = viewRect.y();
    int width = viewRect.width();
    int height = viewRect.height();
    TilesManager* tilesManager = TilesManager::instance();

    // Make sure GL resources are created on the UI thread.
    // They are created either for the first time, or after EGL context
    // recreation caused by onTrimMemory in the framework.
    ShaderProgram* shader = tilesManager->shader();
    if (shader->needsInit()) {
        ALOGD("Reinit shader");
        shader->initGLResources();
    }
    TransferQueue* transferQueue = tilesManager->transferQueue();
    if (transferQueue->needsInit()) {
        ALOGD("Reinit transferQueue");
        transferQueue->initGLResources(TilesManager::tileWidth(),
                                       TilesManager::tileHeight());
    }
    // TODO: Add the video GL resource re-initialization code here.

    shader->setupDrawing(viewRect, visibleRect, webViewRect,
                         titleBarHeight, screenClip, scale);
    shader->calculateAnimationDelta();

    glViewport(left + shader->getAnimationDeltaX(),
               top - shader->getAnimationDeltaY(),
               width, height);

    double currentTime = WTF::currentTime();

    setViewport(visibleRect, scale);

    return currentTime;
}

bool GLWebViewState::setLayersRenderingMode(TexturesResult& nbTexturesNeeded)
{
    bool invalBase = false;

    if (!nbTexturesNeeded.full)
        TilesManager::instance()->setMaxLayerTextureCount(0);
    else
        TilesManager::instance()->setMaxLayerTextureCount((2*nbTexturesNeeded.full)+1);

    int maxTextures = TilesManager::instance()->maxLayerTextureCount();
    LayersRenderingMode layersRenderingMode = m_layersRenderingMode;

    if (m_layersRenderingMode == kSingleSurfaceRendering) {
        // only switch out of SingleSurface mode, if we have 2x needed textures
        // to avoid changing too often
        maxTextures /= 2;
    }

    m_layersRenderingMode = kSingleSurfaceRendering;
    if (nbTexturesNeeded.fixed < maxTextures)
        m_layersRenderingMode = kFixedLayers;
    if (nbTexturesNeeded.scrollable < maxTextures)
        m_layersRenderingMode = kScrollableAndFixedLayers;
    if (nbTexturesNeeded.clipped < maxTextures)
        m_layersRenderingMode = kClippedTextures;
    if (nbTexturesNeeded.full < maxTextures)
        m_layersRenderingMode = kAllTextures;

    if (!maxTextures && !nbTexturesNeeded.full)
        m_layersRenderingMode = kAllTextures;

    if (m_layersRenderingMode < layersRenderingMode
        && m_layersRenderingMode != kAllTextures)
        invalBase = true;

    if (m_layersRenderingMode > layersRenderingMode
        && m_layersRenderingMode != kClippedTextures)
        invalBase = true;

#ifdef DEBUG
    if (m_layersRenderingMode != layersRenderingMode) {
        char* mode[] = { "kAllTextures", "kClippedTextures",
            "kScrollableAndFixedLayers", "kFixedLayers", "kSingleSurfaceRendering" };
        ALOGD("Change from mode %s to %s -- We need textures: fixed: %d,"
              " scrollable: %d, clipped: %d, full: %d, max textures: %d",
              static_cast<char*>(mode[layersRenderingMode]),
              static_cast<char*>(mode[m_layersRenderingMode]),
              nbTexturesNeeded.fixed,
              nbTexturesNeeded.scrollable,
              nbTexturesNeeded.clipped,
              nbTexturesNeeded.full, maxTextures);
    }
#endif

    // For now, anything below kClippedTextures is equivalent
    // to kSingleSurfaceRendering
    // TODO: implement the other rendering modes
    if (m_layersRenderingMode > kClippedTextures)
        m_layersRenderingMode = kSingleSurfaceRendering;

    // update the base surface if needed
    // TODO: inval base layergroup when going into single surface mode
    return (m_layersRenderingMode != layersRenderingMode && invalBase);
}

int GLWebViewState::drawGL(IntRect& rect, SkRect& viewport, IntRect* invalRect,
                           IntRect& webViewRect, int titleBarHeight,
                           IntRect& clip, float scale,
                           bool* collectionsSwappedPtr, bool* newCollectionHasAnimPtr,
                           bool shouldDraw)
{
    TilesManager* tilesManager = TilesManager::instance();
    if (shouldDraw)
        tilesManager->getProfiler()->nextFrame(viewport.fLeft, viewport.fTop,
                                               viewport.fRight, viewport.fBottom,
                                               scale);
    tilesManager->incDrawGLCount();

    ALOGV("drawGL, rect(%d, %d, %d, %d), viewport(%.2f, %.2f, %.2f, %.2f)",
          rect.x(), rect.y(), rect.width(), rect.height(),
          viewport.fLeft, viewport.fTop, viewport.fRight, viewport.fBottom);

    ALOGV("drawGL, invalRect(%d, %d, %d, %d), webViewRect(%d, %d, %d, %d)"
          "clip (%d, %d, %d, %d), scale %f",
          invalRect->x(), invalRect->y(), invalRect->width(), invalRect->height(),
          webViewRect.x(), webViewRect.y(), webViewRect.width(), webViewRect.height(),
          clip.x(), clip.y(), clip.width(), clip.height(), scale);

    resetLayersDirtyArea();

    if (scale < MIN_SCALE_WARNING || scale > MAX_SCALE_WARNING)
        ALOGW("WARNING, scale seems corrupted before update: %e", scale);

    // Here before we draw, update the Tile which has updated content.
    // Inside this function, just do GPU blits from the transfer queue into
    // the Tiles' texture.
    tilesManager->transferQueue()->updateDirtyTiles();

    // Upload any pending ImageTexture
    // Return true if we still have some images to upload.
    // TODO: upload as many textures as possible within a certain time limit
    int returnFlags = 0;
    if (ImagesManager::instance()->prepareTextures(this))
        returnFlags |= uirenderer::DrawGlInfo::kStatusDraw;

    if (scale < MIN_SCALE_WARNING || scale > MAX_SCALE_WARNING) {
        ALOGW("WARNING, scale seems corrupted after update: %e", scale);
        CRASH();
    }

    // gather the textures we can use
    tilesManager->gatherTextures();

    double currentTime = setupDrawing(rect, viewport, webViewRect, titleBarHeight, clip, scale);

    TexturesResult nbTexturesNeeded;
    bool fastSwap = isScrolling() || m_layersRenderingMode == kSingleSurfaceRendering;
    m_glExtras.setViewport(viewport);
    returnFlags |= m_surfaceCollectionManager.drawGL(currentTime, rect, viewport,
                                                     scale, fastSwap,
                                                     collectionsSwappedPtr, newCollectionHasAnimPtr,
                                                     &nbTexturesNeeded, shouldDraw);

    int nbTexturesForImages = ImagesManager::instance()->nbTextures();
    ALOGV("*** We have %d textures for images, %d full, %d clipped, total %d / %d",
          nbTexturesForImages, nbTexturesNeeded.full, nbTexturesNeeded.clipped,
          nbTexturesNeeded.full + nbTexturesForImages,
          nbTexturesNeeded.clipped + nbTexturesForImages);
    nbTexturesNeeded.full += nbTexturesForImages;
    nbTexturesNeeded.clipped += nbTexturesForImages;

    if (setLayersRenderingMode(nbTexturesNeeded))
        returnFlags |= uirenderer::DrawGlInfo::kStatusDraw | uirenderer::DrawGlInfo::kStatusInvoke;

    glBindBuffer(GL_ARRAY_BUFFER, 0);

    // Clean up GL textures for video layer.
    tilesManager->videoLayerManager()->deleteUnusedTextures();

    if (returnFlags & uirenderer::DrawGlInfo::kStatusDraw) {
        // returnFlags & kStatusDraw && empty inval region means we've inval'd everything,
        // but don't have new content. Keep redrawing full view (0,0,0,0)
        // until tile generation catches up and we swap pages.
        bool fullScreenInval = m_frameworkLayersInval.isEmpty();

        if (!fullScreenInval) {
            m_frameworkLayersInval.inflate(1);

            invalRect->setX(m_frameworkLayersInval.x());
            invalRect->setY(m_frameworkLayersInval.y());
            invalRect->setWidth(m_frameworkLayersInval.width());
            invalRect->setHeight(m_frameworkLayersInval.height());

            ALOGV("invalRect(%d, %d, %d, %d)", inval.x(),
                  inval.y(), inval.width(), inval.height());

            if (!invalRect->intersects(rect)) {
                // invalidate is occurring offscreen, do full inval to guarantee redraw
                fullScreenInval = true;
            }
        }

        if (fullScreenInval) {
            invalRect->setX(0);
            invalRect->setY(0);
            invalRect->setWidth(0);
            invalRect->setHeight(0);
        }
    }

    if (shouldDraw)
        showFrameInfo(rect, *collectionsSwappedPtr);

    return returnFlags;
}

void GLWebViewState::showFrameInfo(const IntRect& rect, bool collectionsSwapped)
{
    bool showVisualIndicator = TilesManager::instance()->getShowVisualIndicator();

    bool drawOrDumpFrameInfo = showVisualIndicator;
#ifdef MEASURES_PERF
    drawOrDumpFrameInfo |= m_measurePerfs;
#endif
    if (!drawOrDumpFrameInfo)
        return;

    double currentDrawTime = WTF::currentTime();
    double delta = currentDrawTime - m_prevDrawTime;
    m_prevDrawTime = currentDrawTime;

#ifdef MEASURES_PERF
    if (m_measurePerfs) {
        m_delayTimes[m_timeCounter++] = delta;
        if (m_timeCounter >= MAX_MEASURES_PERF)
            dumpMeasures();
    }
#endif

    IntRect frameInfoRect = rect;
    frameInfoRect.setHeight(FPS_INDICATOR_HEIGHT);
    double ratio = (1.0 / delta) / MAX_FPS_VALUE;

    clearRectWithColor(frameInfoRect, 1, 1, 1, 1);
    frameInfoRect.setWidth(frameInfoRect.width() * ratio);
    clearRectWithColor(frameInfoRect, 1, 0, 0, 1);

    // Draw the collection swap counter as a circling progress bar.
    // This will basically show how fast we are updating the collection.
    static int swappedCounter = 0;
    if (collectionsSwapped)
        swappedCounter = (swappedCounter + 1) % COLLECTION_SWAPPED_COUNTER_MODULE;

    frameInfoRect = rect;
    frameInfoRect.setHeight(FPS_INDICATOR_HEIGHT);
    frameInfoRect.move(0, FPS_INDICATOR_HEIGHT);

    clearRectWithColor(frameInfoRect, 1, 1, 1, 1);
    ratio = (swappedCounter + 1.0) / COLLECTION_SWAPPED_COUNTER_MODULE;

    frameInfoRect.setWidth(frameInfoRect.width() * ratio);
    clearRectWithColor(frameInfoRect, 0, 1, 0, 1);
}

void GLWebViewState::clearRectWithColor(const IntRect& rect, float r, float g,
                                      float b, float a)
{
    glScissor(rect.x(), rect.y(), rect.width(), rect.height());
    glClearColor(r, g, b, a);
    glClear(GL_COLOR_BUFFER_BIT);
}

} // namespace WebCore

#endif // USE(ACCELERATED_COMPOSITING)

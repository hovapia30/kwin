/*
    SPDX-FileCopyrightText: 2010, 2012 Martin Gräßlin <mgraesslin@kde.org>
    SPDX-FileCopyrightText: 2020 Vlad Zahorodnii <vlad.zahorodnii@kde.org>

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "x11_standalone_egl_backend.h"
#include "compositor.h"
#include "core/outputbackend.h"
#include "core/outputlayer.h"
#include "core/overlaywindow.h"
#include "main.h"
#include "opengl/eglcontext.h"
#include "opengl/egldisplay.h"
#include "opengl/glplatform.h"
#include "opengl/glrendertimequery.h"
#include "options.h"
#include "scene/surfaceitem_x11.h"
#include "utils/c_ptr.h"
#include "utils/softwarevsyncmonitor.h"
#include "workspace.h"
#include "x11_standalone_backend.h"
#include "x11_standalone_logging.h"
#include "x11_standalone_overlaywindow.h"

#include <QOpenGLContext>
#include <drm_fourcc.h>

#include <xcb/shm.h>
#include <xcb/xcb.h>

#include <sys/ipc.h>
#include <sys/shm.h>
#include <unistd.h>

#include <algorithm>
#include <cstring>
#include <vector>

namespace KWin
{

EglLayer::EglLayer(EglBackend *backend)
    : OutputLayer(nullptr)
    , m_backend(backend)
{
}

std::optional<OutputLayerBeginFrameInfo> EglLayer::doBeginFrame()
{
    return m_backend->beginFrame();
}

bool EglLayer::doEndFrame(const QRegion &renderedRegion, const QRegion &damagedRegion, OutputFrame *frame)
{
    m_backend->endFrame(renderedRegion, damagedRegion, frame);
    return true;
}

DrmDevice *EglLayer::scanoutDevice() const
{
    return nullptr;
}

QHash<uint32_t, QList<uint64_t>> EglLayer::supportedDrmFormats() const
{
    return {};
}

EglBackend::EglBackend(::Display *display, X11StandaloneBackend *backend)
    : m_backend(backend)
    , m_overlayWindow(std::make_unique<OverlayWindowX11>(backend))
    , m_layer(std::make_unique<EglLayer>(this))
{
    // There is no any way to determine when a buffer swap completes with EGL. Fallback
    // to software vblank events. Could we use the Present extension to get notified when
    // the overlay window is actually presented on the screen?
    m_vsyncMonitor = SoftwareVsyncMonitor::create();
    connect(backend->renderLoop(), &RenderLoop::refreshRateChanged, this, [this, backend]() {
        m_vsyncMonitor->setRefreshRate(backend->renderLoop()->refreshRate());
    });
    m_vsyncMonitor->setRefreshRate(backend->renderLoop()->refreshRate());

    connect(m_vsyncMonitor.get(), &VsyncMonitor::vblankOccurred, this, &EglBackend::vblank);
    Q_ASSERT(workspace());
    connect(workspace(), &Workspace::geometryChanged, this, &EglBackend::screenGeometryChanged);
    overlayWindow()->resize(workspace()->geometry().size());
}

EglBackend::~EglBackend()
{
    m_query.reset();

    if (isFailed() && m_overlayWindow) {
        m_overlayWindow->destroy();
    }
    if (m_surface != EGL_NO_SURFACE) {
        eglDestroySurface(eglDisplayObject()->handle(), m_surface);
    }
    m_context.reset();

    if (m_overlayWindow && m_overlayWindow->window()) {
        m_overlayWindow->destroy();
    }
}

std::unique_ptr<SurfaceTexture> EglBackend::createSurfaceTextureX11(SurfacePixmapX11 *texture)
{
    return std::make_unique<EglSurfaceTextureX11>(this, texture);
}

void EglBackend::init()
{
    QOpenGLContext *qtShareContext = QOpenGLContext::globalShareContext();
    ::EGLDisplay shareDisplay = EGL_NO_DISPLAY;
    ::EGLContext shareContext = EGL_NO_CONTEXT;
    bool haveQtEglShareContext = false;
    if (qtShareContext) {
        qDebug(KWIN_X11STANDALONE) << "Global share context format:" << qtShareContext->format();
        const auto nativeHandle = qtShareContext->nativeInterface<QNativeInterface::QEGLContext>();
        if (nativeHandle) {
            shareContext = nativeHandle->nativeContext();
            shareDisplay = nativeHandle->display();
            if (shareDisplay != EGL_NO_DISPLAY && shareContext != EGL_NO_CONTEXT) {
                haveQtEglShareContext = true;
            }
        }
    }

    m_fbo = std::make_unique<GLFramebuffer>(0, workspace()->geometry().size());

    if (haveQtEglShareContext) {
        m_backend->setEglDisplay(EglDisplay::create(shareDisplay, false));
        m_backend->setSceneEglGlobalShareContext(shareContext);
    } else {
        // Qt global share context is not a native QEGLContext (e.g. running without
        // QT_XCB_GL_INTEGRATION=xcb_egl). Initialize EGL directly on the X11 display.
        qputenv("EGL_PLATFORM", "x11");

        initClientExtensions();
        m_havePlatformBase = hasClientExtension(QByteArrayLiteral("EGL_EXT_platform_base"));

        std::unique_ptr<EglDisplay> eglDisplay;
        if (m_havePlatformBase
            && (hasClientExtension(QByteArrayLiteral("EGL_EXT_platform_x11"))
                || hasClientExtension(QByteArrayLiteral("EGL_KHR_platform_x11")))) {
            eglDisplay = EglDisplay::create(
                eglGetPlatformDisplayEXT(EGL_PLATFORM_X11_EXT, m_backend->display(), nullptr), true);
        }
        if (!eglDisplay) {
            eglDisplay = EglDisplay::create(eglGetDisplay(m_backend->display()), true);
        }
        if (!eglDisplay) {
            setFailed(QStringLiteral("Failed to create EGL display for direct initialization"));
            return;
        }
        m_backend->setEglDisplay(std::move(eglDisplay));
        m_backend->setSceneEglGlobalShareContext(EGL_NO_CONTEXT);
    }

    qputenv("EGL_PLATFORM", "x11");
    if (!initRenderingContext()) {
        setFailed(QStringLiteral("Could not initialize rendering context"));
        return;
    }

    // Ensure a valid EGL share context is published for the rest of the stack
    // (e.g. QPA/platform integration) when running without a Qt QEGLContext.
    if (m_backend->sceneEglGlobalShareContext() == EGL_NO_CONTEXT && m_context) {
        m_backend->setSceneEglGlobalShareContext(m_context->handle());
    }

    if (!hasExtension(QByteArrayLiteral("EGL_KHR_image")) && (!hasExtension(QByteArrayLiteral("EGL_KHR_image_base")) || !hasExtension(QByteArrayLiteral("EGL_KHR_image_pixmap")))) {
        setFailed(QStringLiteral("Required support for binding pixmaps to EGLImages not found, disabling compositing"));
        return;
    }
    if (!m_context->hasOpenglExtension(QByteArrayLiteral("GL_OES_EGL_image"))) {
        setFailed(QStringLiteral("Required extension GL_OES_EGL_image not found, disabling compositing"));
        return;
    }

    // check for EGL_NV_post_sub_buffer and whether it can be used on the surface
    if (hasExtension(QByteArrayLiteral("EGL_NV_post_sub_buffer"))) {
        if (eglQuerySurface(eglDisplayObject()->handle(), m_surface, EGL_POST_SUB_BUFFER_SUPPORTED_NV, &m_havePostSubBuffer) == EGL_FALSE) {
            EGLint error = eglGetError();
            if (error != EGL_SUCCESS && error != EGL_BAD_ATTRIBUTE) {
                setFailed(QStringLiteral("query surface failed"));
                return;
            } else {
                m_havePostSubBuffer = EGL_FALSE;
            }
        }
    }

    if (m_havePostSubBuffer) {
        qCDebug(KWIN_CORE) << "EGL implementation and surface support eglPostSubBufferNV, let's use it";

        // check if swap interval 1 is supported
        EGLint val;
        eglGetConfigAttrib(eglDisplayObject()->handle(), m_context->config(), EGL_MAX_SWAP_INTERVAL, &val);
        if (val >= 1) {
            if (eglSwapInterval(eglDisplayObject()->handle(), 1)) {
                qCDebug(KWIN_CORE) << "Enabled v-sync";
            }
        } else {
            qCWarning(KWIN_CORE) << "Cannot enable v-sync as max. swap interval is" << val;
        }
    } else {
        /* In the GLX backend, we fall back to using glCopyPixels if we have no extension providing support for partial screen updates.
         * However, that does not work in EGL - glCopyPixels with glDrawBuffer(GL_FRONT); does nothing.
         * Hence we need EGL to preserve the backbuffer for us, so that we can draw the partial updates on it and call
         * eglSwapBuffers() for each frame. eglSwapBuffers() then does the copy (no page flip possible in this mode),
         * which means it is slow and not synced to the v-blank. */
        qCWarning(KWIN_CORE) << "eglPostSubBufferNV not supported, have to enable buffer preservation - which breaks v-sync and performance";
        eglSurfaceAttrib(eglDisplayObject()->handle(), m_surface, EGL_SWAP_BEHAVIOR, EGL_BUFFER_PRESERVED);
    }

    m_swapStrategy = options->glPreferBufferSwap();
    if (m_swapStrategy == Options::AutoSwapStrategy) {
        // buffer copying is very fast with the nvidia blob
        // but due to restrictions in DRI2 *incredibly* slow for all MESA drivers
        // see https://www.x.org/releases/X11R7.7/doc/dri2proto/dri2proto.txt, item 2.5
        if (m_context->glPlatform()->driver() == Driver_NVidia) {
            m_swapStrategy = Options::CopyFrontBuffer;
        } else if (m_context->glPlatform()->driver() != Driver_Unknown) { // undetected, finally resolved when context is initialized
            m_swapStrategy = Options::ExtendDamage;
        }
    }
}

void EglBackend::initClientExtensions()
{
    // Get the list of client extensions
    const char *clientExtensionsCString = eglQueryString(EGL_NO_DISPLAY, EGL_EXTENSIONS);
    const QByteArray clientExtensionsString = QByteArray::fromRawData(clientExtensionsCString, qstrlen(clientExtensionsCString));
    if (clientExtensionsString.isEmpty()) {
        // If eglQueryString() returned NULL, the implementation doesn't support
        // EGL_EXT_client_extensions. Expect an EGL_BAD_DISPLAY error.
        (void)eglGetError();
    }

    m_clientExtensions = clientExtensionsString.split(' ');
}

bool EglBackend::hasClientExtension(const QByteArray &name)
{
    return m_clientExtensions.contains(name);
}

bool EglBackend::initRenderingContext()
{
    initClientExtensions();
    auto display = m_backend->sceneEglDisplayObject();

    // Use eglGetPlatformDisplayEXT() to get the display pointer
    // if the implementation supports it.
    if (!display) {
        m_havePlatformBase = hasClientExtension(QByteArrayLiteral("EGL_EXT_platform_base"));
        if (m_havePlatformBase) {
            // Make sure that the X11 platform is supported
            if (!hasClientExtension(QByteArrayLiteral("EGL_EXT_platform_x11")) && !hasClientExtension(QByteArrayLiteral("EGL_KHR_platform_x11"))) {
                qCWarning(KWIN_CORE) << "EGL_EXT_platform_base is supported, but neither EGL_EXT_platform_x11 nor EGL_KHR_platform_x11 is supported."
                                     << "Cannot create EGLDisplay on X11";
                return false;
            }

            m_backend->setEglDisplay(EglDisplay::create(eglGetPlatformDisplayEXT(EGL_PLATFORM_X11_EXT, m_backend->display(), nullptr)));
        } else {
            m_backend->setEglDisplay(EglDisplay::create(eglGetDisplay(m_backend->display())));
        }
        display = m_backend->sceneEglDisplayObject();
        if (!display) {
            qCWarning(KWIN_CORE) << "Failed to get the EGLDisplay";
            return false;
        }
    }

    setSupportsBufferAge(display->supportsBufferAge());
    setSupportsNativeFence(display->supportsNativeFence());
    setExtensions(display->extensions());
    m_context = EglContext::create(display, chooseBufferConfig(), m_backend->sceneEglGlobalShareContext());
    if (!m_context) {
        qCCritical(KWIN_CORE) << "Create OpenGL context failed";
        return false;
    }

    if (!m_overlayWindow->create()) {
        qCCritical(KWIN_X11STANDALONE) << "Could not get overlay window";
        return false;
    } else {
        m_overlayWindow->setup(XCB_WINDOW_NONE);
    }

    // Ensure the overlay window is known to the X server before Adreno's EGL
    // subdriver queries it (e.g. via xcb_get_geometry on its own connection).
    if (m_backend->connection()) {
        xcb_flush(m_backend->connection());
    }

    m_surface = createSurface(m_overlayWindow->window());
    if (m_surface == EGL_NO_SURFACE) {
        qCCritical(KWIN_CORE) << "Creating egl surface failed";
        return false;
    }

    if (!makeCurrent()) {
        qCCritical(KWIN_CORE) << "Make Context Current failed";
        return false;
    }

    EGLint error = eglGetError();
    if (error != EGL_SUCCESS) {
        qCWarning(KWIN_CORE) << "Error occurred while creating context " << error;
        return false;
    }

    return true;
}

EGLSurface EglBackend::createSurface(xcb_window_t window)
{
    if (window == XCB_WINDOW_NONE) {
        return EGL_NO_SURFACE;
    }

    // Ensure the window XID is realized server-side before the EGL driver queries it.
    if (m_backend->connection()) {
        xcb_flush(m_backend->connection());
    }

    // Window is 64 bits on a 64-bit architecture whereas xcb_window_t is always 32 bits.
    ::Window nativeWindow = window;

    EGLSurface surface = EGL_NO_SURFACE;

    // Some proprietary Adreno EGL X11 drivers expect the plain X11 Window XID via
    // eglCreateWindowSurface() and may fail internal geometry queries with
    // eglCreatePlatformWindowSurfaceEXT(). Try the direct path first.
    surface = eglCreateWindowSurface(eglDisplayObject()->handle(), m_context->config(),
                                     (EGLNativeWindowType)(uintptr_t)nativeWindow, nullptr);
    if (surface != EGL_NO_SURFACE) {
        return surface;
    }
    if (eglGetError() != EGL_SUCCESS) {
        // clear error state before retrying with platform API
    }

    if (m_havePlatformBase) {
        // eglCreatePlatformWindowSurfaceEXT() expects a pointer to the Window.
        surface = eglCreatePlatformWindowSurfaceEXT(eglDisplayObject()->handle(), m_context->config(),
                                                    (void *)&nativeWindow, nullptr);
        if (surface != EGL_NO_SURFACE) {
            return surface;
        }
    }

    return EGL_NO_SURFACE;
}

EGLConfig EglBackend::chooseBufferConfig()
{
    const EGLint config_attribs[] = {
        EGL_SURFACE_TYPE,
        EGL_WINDOW_BIT | (supportsBufferAge() ? 0 : EGL_SWAP_BEHAVIOR_PRESERVED_BIT),
        EGL_RED_SIZE,
        1,
        EGL_GREEN_SIZE,
        1,
        EGL_BLUE_SIZE,
        1,
        EGL_ALPHA_SIZE,
        0,
        EGL_RENDERABLE_TYPE,
        EglDisplay::shouldUseOpenGLES() ? EGL_OPENGL_ES2_BIT : EGL_OPENGL_BIT,
        EGL_CONFIG_CAVEAT,
        EGL_NONE,
        EGL_NONE,
    };

    EGLint count = 0;
    EGLConfig configs[1024];
    if (eglChooseConfig(eglDisplayObject()->handle(), config_attribs, configs, 1024, &count) == EGL_FALSE || count == 0) {
        // Retry without EGL_CONFIG_CAVEAT in case the driver only exposes
        // slow/non-conformant configs (e.g. some proprietary EGL stacks).
        const EGLint relaxed_attribs[] = {
            EGL_SURFACE_TYPE,
            EGL_WINDOW_BIT | (supportsBufferAge() ? 0 : EGL_SWAP_BEHAVIOR_PRESERVED_BIT),
            EGL_RED_SIZE,
            1,
            EGL_GREEN_SIZE,
            1,
            EGL_BLUE_SIZE,
            1,
            EGL_ALPHA_SIZE,
            0,
            EGL_RENDERABLE_TYPE,
            EglDisplay::shouldUseOpenGLES() ? EGL_OPENGL_ES2_BIT : EGL_OPENGL_BIT,
            EGL_NONE,
        };
        count = 0;
        if (eglChooseConfig(eglDisplayObject()->handle(), relaxed_attribs, configs, 1024, &count) == EGL_FALSE || count == 0) {
            qCCritical(KWIN_CORE) << "choose config failed";
            return EGL_NO_CONFIG_KHR;
        }
    }

    UniqueCPtr<xcb_get_window_attributes_reply_t> attribs(xcb_get_window_attributes_reply(m_backend->connection(),
                                                                                          xcb_get_window_attributes_unchecked(m_backend->connection(), m_backend->rootWindow()),
                                                                                          nullptr));
    if (!attribs) {
        qCCritical(KWIN_CORE) << "Failed to get window attributes of root window";
        // Prefer a 32-bit RGBA or 24-bit RGB config if available.
        for (int i = 0; i < count; i++) {
            EGLint r = 0, g = 0, b = 0, a = 0;
            eglGetConfigAttrib(eglDisplayObject()->handle(), configs[i], EGL_RED_SIZE, &r);
            eglGetConfigAttrib(eglDisplayObject()->handle(), configs[i], EGL_GREEN_SIZE, &g);
            eglGetConfigAttrib(eglDisplayObject()->handle(), configs[i], EGL_BLUE_SIZE, &b);
            eglGetConfigAttrib(eglDisplayObject()->handle(), configs[i], EGL_ALPHA_SIZE, &a);
            if ((r == 8 && g == 8 && b == 8 && (a == 8 || a == 0))) {
                return configs[i];
            }
        }
        return configs[0];
    }

    for (int i = 0; i < count; i++) {
        EGLint val = 0;
        if (eglGetConfigAttrib(eglDisplayObject()->handle(), configs[i], EGL_NATIVE_VISUAL_ID, &val) == EGL_FALSE) {
            qCCritical(KWIN_CORE) << "egl get config attrib failed";
        }
        if (uint32_t(val) == attribs->visual) {
            return configs[i];
        }
    }

    // Some EGL implementations (e.g. Adreno proprietary on X11) may not expose a
    // native visual ID that matches the compositor/root window visual. Fall back to
    // a suitable 32-bit RGBA or 24-bit RGB config instead of requiring an exact match.
    for (int i = 0; i < count; i++) {
        EGLint r = 0, g = 0, b = 0, a = 0;
        eglGetConfigAttrib(eglDisplayObject()->handle(), configs[i], EGL_RED_SIZE, &r);
        eglGetConfigAttrib(eglDisplayObject()->handle(), configs[i], EGL_GREEN_SIZE, &g);
        eglGetConfigAttrib(eglDisplayObject()->handle(), configs[i], EGL_BLUE_SIZE, &b);
        eglGetConfigAttrib(eglDisplayObject()->handle(), configs[i], EGL_ALPHA_SIZE, &a);
        if (r == 8 && g == 8 && b == 8 && a == 8) {
            return configs[i];
        }
    }
    for (int i = 0; i < count; i++) {
        EGLint r = 0, g = 0, b = 0, a = 0;
        eglGetConfigAttrib(eglDisplayObject()->handle(), configs[i], EGL_RED_SIZE, &r);
        eglGetConfigAttrib(eglDisplayObject()->handle(), configs[i], EGL_GREEN_SIZE, &g);
        eglGetConfigAttrib(eglDisplayObject()->handle(), configs[i], EGL_BLUE_SIZE, &b);
        eglGetConfigAttrib(eglDisplayObject()->handle(), configs[i], EGL_ALPHA_SIZE, &a);
        if (r == 8 && g == 8 && b == 8 && a == 0) {
            return configs[i];
        }
    }
    return configs[0];
}

void EglBackend::screenGeometryChanged()
{
    overlayWindow()->resize(workspace()->geometry().size());

    // The back buffer contents are now undefined
    m_bufferAge = 0;
    m_fbo = std::make_unique<GLFramebuffer>(0, workspace()->geometry().size());
}

OutputLayerBeginFrameInfo EglBackend::beginFrame()
{
    makeCurrent();

    QRegion repaint;
    if (supportsBufferAge()) {
        repaint = m_damageJournal.accumulate(m_bufferAge, infiniteRegion());
    }
    eglWaitNative(EGL_CORE_NATIVE_ENGINE);

    m_query = std::make_unique<GLRenderTimeQuery>(m_context);
    m_query->begin();
    return OutputLayerBeginFrameInfo{
        .renderTarget = RenderTarget(m_fbo.get()),
        .repaint = repaint,
    };
}

void EglBackend::endFrame(const QRegion &renderedRegion, const QRegion &damagedRegion, OutputFrame *frame)
{
    m_query->end();
    frame->addRenderTimeQuery(std::move(m_query));
    // Save the damaged region to history
    if (supportsBufferAge()) {
        m_damageJournal.add(damagedRegion);
    }
    m_lastRenderedRegion = renderedRegion;
}

bool EglBackend::present(Output *output, const std::shared_ptr<OutputFrame> &frame)
{
    m_frame = frame;
    // Start the software vsync monitor. There is no any reliable way to determine when
    // eglSwapBuffers() or eglSwapBuffersWithDamageEXT() completes.
    m_vsyncMonitor->arm();

    QRegion effectiveRenderedRegion = m_lastRenderedRegion;
    if (!m_context->isOpenGLES()) {
        const QRect displayRect = workspace()->geometry();
        if (!supportsBufferAge() && m_swapStrategy == Options::CopyFrontBuffer && m_lastRenderedRegion != displayRect) {
            glReadBuffer(GL_FRONT);
            copyPixels(QRegion(displayRect) - m_lastRenderedRegion, displayRect.size());
            glReadBuffer(GL_BACK);
            effectiveRenderedRegion = displayRect;
        }
    }

    presentSurface(m_surface, effectiveRenderedRegion, workspace()->geometry());

    if (overlayWindow() && overlayWindow()->window()) { // show the window only after the first pass,
        overlayWindow()->show(); // since that pass may take long
    }
    return true;
}

void EglBackend::presentSurface(EGLSurface surface, const QRegion &damage, const QRect &screenGeometry)
{
    const bool fullRepaint = supportsBufferAge() || (damage == screenGeometry);

    if (fullRepaint || !m_havePostSubBuffer) {
        // the entire screen changed, or we cannot do partial updates (which implies we enabled surface preservation)
        eglSwapBuffers(eglDisplayObject()->handle(), surface);
        if (supportsBufferAge()) {
            eglQuerySurface(eglDisplayObject()->handle(), surface, EGL_BUFFER_AGE_EXT, &m_bufferAge);
        }
    } else {
        // a part of the screen changed, and we can use eglPostSubBufferNV to copy the updated area
        for (const QRect &r : damage) {
            eglPostSubBufferNV(eglDisplayObject()->handle(), surface, r.left(), screenGeometry.height() - r.bottom() - 1, r.width(), r.height());
        }
    }
}

OverlayWindow *EglBackend::overlayWindow() const
{
    return m_overlayWindow.get();
}

OutputLayer *EglBackend::primaryLayer(Output *output)
{
    return m_layer.get();
}

void EglBackend::vblank(std::chrono::nanoseconds timestamp)
{
    m_frame->presented(timestamp, PresentationMode::VSync);
    m_frame.reset();
}

EglDisplay *EglBackend::eglDisplayObject() const
{
    return m_backend->sceneEglDisplayObject();
}

OpenGlContext *EglBackend::openglContext() const
{
    return m_context.get();
}

bool EglBackend::makeCurrent()
{
    return m_context->makeCurrent(m_surface);
}

void EglBackend::doneCurrent()
{
    m_context->doneCurrent();
}

EglSurfaceTextureX11::EglSurfaceTextureX11(EglBackend *backend, SurfacePixmapX11 *texture)
    : OpenGLSurfaceTextureX11(backend, texture)
{
}

bool EglSurfaceTextureX11::create()
{
    auto texture = std::make_shared<EglPixmapTexture>(static_cast<EglBackend *>(m_backend));
    if (texture->create(m_pixmap)) {
        m_texture = {texture};
        return true;
    } else {
        return false;
    }
}

void EglSurfaceTextureX11::update(const QRegion &region)
{
    if (m_texture.isValid() && !m_texture.planes.isEmpty()) {
        if (auto pixmapTexture = std::dynamic_pointer_cast<EglPixmapTexture>(m_texture.planes.first())) {
            if (pixmapTexture->isFallback()) {
                pixmapTexture->updateFromPixmap(m_pixmap, region);
            }
        }
    }
    // mipmaps need to be updated
    m_texture.setDirty();
}

EglPixmapTexture::EglPixmapTexture(EglBackend *backend)
    : GLTexture(GL_TEXTURE_2D)
    , m_backend(backend)
{
}

EglPixmapTexture::~EglPixmapTexture()
{
    if (m_image != EGL_NO_IMAGE_KHR) {
        eglDestroyImageKHR(m_backend->eglDisplayObject()->handle(), m_image);
    }
}

bool EglPixmapTexture::isFallback() const
{
    return m_isFallback;
}

bool EglPixmapTexture::create(SurfacePixmapX11 *pixmap)
{
    const xcb_pixmap_t nativePixmap = pixmap->pixmap();
    if (nativePixmap == XCB_NONE) {
        return false;
    }

    glGenTextures(1, &d->m_texture);
    setWrapMode(GL_CLAMP_TO_EDGE);
    setFilter(GL_LINEAR);
    bind();

    const EGLint attribs[] = {
        EGL_IMAGE_PRESERVED_KHR, EGL_TRUE,
        EGL_NONE};
    m_image = eglCreateImageKHR(m_backend->eglDisplayObject()->handle(),
                                EGL_NO_CONTEXT,
                                EGL_NATIVE_PIXMAP_KHR,
                                reinterpret_cast<EGLClientBuffer>(static_cast<uintptr_t>(nativePixmap)),
                                attribs);

    if (m_image != EGL_NO_IMAGE_KHR) {
        glEGLImageTargetTexture2DOES(GL_TEXTURE_2D, static_cast<GLeglImageOES>(m_image));
        unbind();
        setContentTransform(OutputTransform::FlipY);
        d->m_size = pixmap->size();
        d->updateMatrix();
        m_isFallback = false;
        m_pixmapSize = pixmap->size();
        return true;
    }

    if (!loadFallback(pixmap)) {
        unbind();
        return false;
    }

    unbind();
    setContentTransform(OutputTransform::FlipY);
    d->m_size = pixmap->size();
    d->updateMatrix();
    m_isFallback = true;
    m_pixmapSize = pixmap->size();
    return true;
}

bool EglPixmapTexture::loadFallback(SurfacePixmapX11 *pixmap)
{
    xcb_connection_t *c = kwinApp()->x11Connection();
    if (!c) {
        return false;
    }

    xcb_generic_error_t *err = nullptr;
    UniqueCPtr<xcb_get_geometry_reply_t> geom(xcb_get_geometry_reply(c,
                                                                     xcb_get_geometry_unchecked(c, pixmap->pixmap()),
                                                                     &err));
    if (err || !geom) {
        free(err);
        return false;
    }

    m_depth = geom->depth;
    const int width = geom->width;
    const int height = geom->height;
    if (width <= 0 || height <= 0) {
        return false;
    }

    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    if (!fetchAndUpload(pixmap, 0, 0, width, height)) {
        // keep texture allocated; it may be updated later, but initial load failed
        return false;
    }
    return true;
}

static inline void convertToRGBA(const uint8_t *src, uint8_t *dst, size_t pixelCount, uint8_t depth)
{
    for (size_t i = 0; i < pixelCount; ++i) {
        const uint8_t b = src[i * 4 + 0];
        const uint8_t g = src[i * 4 + 1];
        const uint8_t r = src[i * 4 + 2];
        const uint8_t a = src[i * 4 + 3];
        dst[i * 4 + 0] = r;
        dst[i * 4 + 1] = g;
        dst[i * 4 + 2] = b;
        dst[i * 4 + 3] = (depth == 24) ? 0xFF : a;
    }
}

bool EglPixmapTexture::fetchAndUpload(SurfacePixmapX11 *pixmap, int x, int y, int width, int height)
{
    xcb_connection_t *c = kwinApp()->x11Connection();
    if (!c || width <= 0 || height <= 0) {
        return false;
    }

    const xcb_pixmap_t drawable = pixmap->pixmap();
    if (drawable == XCB_NONE) {
        return false;
    }

    const size_t pixelCount = size_t(width) * size_t(height);
    const size_t byteCount = pixelCount * 4u;
    std::vector<uint8_t> rgba(byteCount);
    bool gotImage = false;

    // Try XShm for faster transfer
    xcb_generic_error_t *shmErr = nullptr;
    int shmId = -1;
    void *shmAddr = reinterpret_cast<void *>(-1);
    xcb_shm_seg_t shmSeg = XCB_NONE;
    if (byteCount > 0) {
        shmId = shmget(IPC_PRIVATE, byteCount, IPC_CREAT | 0600);
        if (shmId >= 0) {
            shmAddr = shmat(shmId, nullptr, 0);
            if (shmAddr != reinterpret_cast<void *>(-1)) {
                shmSeg = xcb_generate_id(c);
                xcb_void_cookie_t attachCookie = xcb_shm_attach_checked(c, shmSeg, shmId, 0);
                shmErr = xcb_request_check(c, attachCookie);
                if (!shmErr) {
                    xcb_shm_get_image_cookie_t imgCookie = xcb_shm_get_image(c, drawable,
                                                                             int16_t(x), int16_t(y),
                                                                             uint16_t(width), uint16_t(height),
                                                                             ~0u, XCB_IMAGE_FORMAT_Z_PIXMAP,
                                                                             shmSeg, 0);
                    UniqueCPtr<xcb_shm_get_image_reply_t> imgReply(xcb_shm_get_image_reply(c, imgCookie, &shmErr));
                    if (!shmErr && imgReply) {
                        const uint8_t *src = static_cast<const uint8_t *>(shmAddr);
                        convertToRGBA(src, rgba.data(), pixelCount, m_depth ? m_depth : imgReply->depth);
                        gotImage = true;
                    }
                    xcb_shm_detach(c, shmSeg);
                }
            }
        }
    }

    if (shmSeg != XCB_NONE || shmErr) {
        // best effort cleanup even if attach/detach state uncertain
    }
    free(shmErr);
    if (shmAddr != reinterpret_cast<void *>(-1)) {
        shmdt(shmAddr);
    }
    if (shmId >= 0) {
        shmctl(shmId, IPC_RMID, nullptr);
    }

    if (!gotImage) {
        xcb_generic_error_t *err = nullptr;
        UniqueCPtr<xcb_get_image_reply_t> reply(xcb_get_image_reply(c,
                                                                    xcb_get_image_unchecked(c, XCB_IMAGE_FORMAT_Z_PIXMAP, drawable,
                                                                                            int16_t(x), int16_t(y),
                                                                                            uint16_t(width), uint16_t(height), ~0u),
                                                                    &err));
        if (err || !reply) {
            free(err);
            return false;
        }
        const uint8_t *src = xcb_get_image_data(reply.get());
        const int dataLen = xcb_get_image_data_length(reply.get());
        const size_t expected = byteCount;
        if (!src || dataLen < int(expected)) {
            return false;
        }
        convertToRGBA(src, rgba.data(), pixelCount, m_depth ? m_depth : reply->depth);
        gotImage = true;
    }

    if (!gotImage) {
        return false;
    }

    bind();
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexSubImage2D(GL_TEXTURE_2D, 0, x, y, width, height, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
    return true;
}

void EglPixmapTexture::updateFromPixmap(SurfacePixmapX11 *pixmap, const QRegion &region)
{
    if (!m_isFallback || !pixmap || pixmap->pixmap() == XCB_NONE) {
        return;
    }

    const QRect bounds(QPoint(0, 0), m_pixmapSize.isValid() ? m_pixmapSize : pixmap->size());
    for (const QRect &rect : region) {
        const QRect clipped = rect.intersected(bounds);
        if (clipped.isEmpty()) {
            continue;
        }
        fetchAndUpload(pixmap, clipped.x(), clipped.y(), clipped.width(), clipped.height());
    }
}

void EglPixmapTexture::onDamage()
{
    if (m_isFallback || m_image == EGL_NO_IMAGE_KHR) {
        return;
    }
    if (options->isGlStrictBinding()) {
        // This is just implemented to be consistent with
        // the example in mesa/demos/src/egl/opengles1/texture_from_pixmap.c
        eglWaitNative(EGL_CORE_NATIVE_ENGINE);
        glEGLImageTargetTexture2DOES(GL_TEXTURE_2D, static_cast<GLeglImageOES>(m_image));
    }
}

} // namespace KWin

#include "moc_x11_standalone_egl_backend.cpp"

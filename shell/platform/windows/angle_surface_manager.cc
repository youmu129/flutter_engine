// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "flutter/shell/platform/windows/angle_surface_manager.h"

#include <vector>

#include "flutter/fml/logging.h"

// Logs an EGL error to stderr. This automatically calls eglGetError()
// and logs the error code.
static void LogEglError(std::string message) {
  EGLint error = eglGetError();
  FML_LOG(ERROR) << "EGL: " << message;
  FML_LOG(ERROR) << "EGL: eglGetError returned " << error;
}

namespace flutter {

namespace {

class RenderTargetDxgiSharedHandle : public AngleRenderTarget {
public:
  RenderTargetDxgiSharedHandle(EGLint width, EGLint height)
    : width_(width), 
      height_(height),
      handle_((HANDLE)0),
      surface_(EGL_NO_SURFACE),
      texture_id_(0) {}
  ~RenderTargetDxgiSharedHandle() {}

public:
  bool Initialize(Microsoft::WRL::ComPtr<ID3D11Device>& d3d11_device) {
    if (!d3d11_device) {
      return false;
    }

    Microsoft::WRL::ComPtr<ID3D11Device> d3d11_device1;
    HRESULT hr = d3d11_device.As(&d3d11_device1);
    if (FAILED(hr)) {
      return false;
    }

    D3D11_TEXTURE2D_DESC td = {0};
    td.ArraySize = 1;
    td.CPUAccessFlags = 0;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.Width = width();
    td.Height = height();
    td.MipLevels = 1;
    td.SampleDesc.Count = 1;
    td.SampleDesc.Quality = 0;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    td.MiscFlags = 0;

    hr = d3d11_device1->CreateTexture2D(&td, nullptr, texture_.GetAddressOf());
    if (FAILED(hr)) {
      return false;
    }
    
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    td.MiscFlags = D3D11_RESOURCE_MISC_SHARED;
    hr = d3d11_device1->CreateTexture2D(&td, nullptr,
                                        staging_texture_.GetAddressOf());
    if (FAILED(hr)) {
      return false;
    }
    
    Microsoft::WRL::ComPtr<IDXGIResource> dxgi_res;
    if (staging_texture_.Get()) {
      hr = staging_texture_.As(&dxgi_res);
    } else {
      hr = texture_.As(&dxgi_res);
    }
    if (SUCCEEDED(hr)) {
      dxgi_res->GetSharedHandle(&handle_);
    }

    return true;
  }
  
  void Lock() {
    // In the future a keyed mutex could be utilized here.
  }

  void Unlock() {
    if (staging_texture_.Get() && texture_.Get()) {
      Microsoft::WRL::ComPtr<ID3D11Device> d3d11_device;
      staging_texture_->GetDevice(&d3d11_device);
      if (d3d11_device.Get()) {
        Microsoft::WRL::ComPtr<ID3D11DeviceContext> d3d11_ctx;
        d3d11_device->GetImmediateContext(&d3d11_ctx);
        if (d3d11_ctx.Get()) {
          d3d11_ctx->CopyResource(staging_texture_.Get(), texture_.Get());
          d3d11_ctx->Flush();
        }
      }
    }
  }

  void SetSurface(EGLSurface surface, GLuint texture_id) {
    surface_ = surface;
    texture_id_ = texture_id;
  }

  EGLint width() const { return width_; }
  EGLint height() const { return height_; }
  ID3D11Texture2D* texture() const { return texture_.Get(); }
  void* shared_handle() const { return handle_; }
  EGLSurface surface() const { return surface_; }
  GLuint texture_id() const { return texture_id_; }

private:
  EGLint width_;
  EGLint height_;
  Microsoft::WRL::ComPtr<ID3D11Texture2D> texture_;
  Microsoft::WRL::ComPtr<ID3D11Texture2D> staging_texture_;
  HANDLE handle_;
  EGLSurface surface_;
  GLuint texture_id_;
};

}

int AngleSurfaceManager::instance_count_ = 0;

std::unique_ptr<AngleSurfaceManager> AngleSurfaceManager::Create() {
  std::unique_ptr<AngleSurfaceManager> manager;
  manager.reset(new AngleSurfaceManager());
  if (!manager->initialize_succeeded_) {
    return nullptr;
  }
  return std::move(manager);
}

AngleSurfaceManager::AngleSurfaceManager()
    : egl_config_(nullptr),
      egl_display_(EGL_NO_DISPLAY),
      egl_context_(EGL_NO_CONTEXT) {
  initialize_succeeded_ = Initialize();
  ++instance_count_;
}

AngleSurfaceManager::~AngleSurfaceManager() {
  CleanUp();
  --instance_count_;
}

bool AngleSurfaceManager::InitializeEGL(
    PFNEGLGETPLATFORMDISPLAYEXTPROC egl_get_platform_display_EXT,
    const EGLint* config,
    bool should_log) {
  egl_display_ = egl_get_platform_display_EXT(EGL_PLATFORM_ANGLE_ANGLE,
                                              EGL_DEFAULT_DISPLAY, config);

  if (egl_display_ == EGL_NO_DISPLAY) {
    if (should_log) {
      LogEglError("Failed to get a compatible EGLdisplay");
    }
    return false;
  }

  if (eglInitialize(egl_display_, nullptr, nullptr) == EGL_FALSE) {
    if (should_log) {
      LogEglError("Failed to initialize EGL via ANGLE");
    }
    return false;
  }

  return true;
}

bool AngleSurfaceManager::Initialize() {
  // TODO(dnfield): Enable MSAA here, see similar code in android_context_gl.cc
  // Will need to plumb in argument from project bundle for sampling rate.
  // https://github.com/flutter/flutter/issues/100392
  const EGLint config_attributes[] = {EGL_RED_SIZE,   8, EGL_GREEN_SIZE,   8,
                                      EGL_BLUE_SIZE,  8, EGL_ALPHA_SIZE,   8,
                                      EGL_DEPTH_SIZE, 8, EGL_STENCIL_SIZE, 8,
                                      EGL_NONE};

  const EGLint display_context_attributes[] = {EGL_CONTEXT_CLIENT_VERSION, 2,
                                               EGL_NONE};

  // These are preferred display attributes and request ANGLE's D3D11
  // renderer. eglInitialize will only succeed with these attributes if the
  // hardware supports D3D11 Feature Level 10_0+.
  const EGLint d3d11_display_attributes[] = {
      EGL_PLATFORM_ANGLE_TYPE_ANGLE,
      EGL_PLATFORM_ANGLE_TYPE_D3D11_ANGLE,

      // EGL_PLATFORM_ANGLE_ENABLE_AUTOMATIC_TRIM_ANGLE is an option that will
      // enable ANGLE to automatically call the IDXGIDevice3::Trim method on
      // behalf of the application when it gets suspended.
      EGL_PLATFORM_ANGLE_ENABLE_AUTOMATIC_TRIM_ANGLE,
      EGL_TRUE,

      // This extension allows angle to render directly on a D3D swapchain
      // in the correct orientation on D3D11.
      EGL_EXPERIMENTAL_PRESENT_PATH_ANGLE,
      EGL_EXPERIMENTAL_PRESENT_PATH_FAST_ANGLE,

      EGL_NONE,
  };

  // These are used to request ANGLE's D3D11 renderer, with D3D11 Feature
  // Level 9_3.
  const EGLint d3d11_fl_9_3_display_attributes[] = {
      EGL_PLATFORM_ANGLE_TYPE_ANGLE,
      EGL_PLATFORM_ANGLE_TYPE_D3D11_ANGLE,
      EGL_PLATFORM_ANGLE_MAX_VERSION_MAJOR_ANGLE,
      9,
      EGL_PLATFORM_ANGLE_MAX_VERSION_MINOR_ANGLE,
      3,
      EGL_PLATFORM_ANGLE_ENABLE_AUTOMATIC_TRIM_ANGLE,
      EGL_TRUE,
      EGL_NONE,
  };

  // These attributes request D3D11 WARP (software rendering fallback) in case
  // hardware-backed D3D11 is unavailable.
  const EGLint d3d11_warp_display_attributes[] = {
      EGL_PLATFORM_ANGLE_TYPE_ANGLE,
      EGL_PLATFORM_ANGLE_TYPE_D3D11_ANGLE,
      EGL_PLATFORM_ANGLE_ENABLE_AUTOMATIC_TRIM_ANGLE,
      EGL_TRUE,
      EGL_NONE,
  };

  std::vector<const EGLint*> display_attributes_configs = {
      d3d11_display_attributes,
      d3d11_fl_9_3_display_attributes,
      d3d11_warp_display_attributes,
  };

  PFNEGLGETPLATFORMDISPLAYEXTPROC egl_get_platform_display_EXT =
      reinterpret_cast<PFNEGLGETPLATFORMDISPLAYEXTPROC>(
          eglGetProcAddress("eglGetPlatformDisplayEXT"));
  if (!egl_get_platform_display_EXT) {
    LogEglError("eglGetPlatformDisplayEXT not available");
    return false;
  }

  // Attempt to initialize ANGLE's renderer in order of: D3D11, D3D11 Feature
  // Level 9_3 and finally D3D11 WARP.
  for (auto config : display_attributes_configs) {
    bool should_log = (config == display_attributes_configs.back());
    if (InitializeEGL(egl_get_platform_display_EXT, config, should_log)) {
      break;
    }
  }

  EGLint numConfigs = 0;
  if ((eglChooseConfig(egl_display_, config_attributes, &egl_config_, 1,
                       &numConfigs) == EGL_FALSE) ||
      (numConfigs == 0)) {
    LogEglError("Failed to choose first context");
    return false;
  }

  egl_context_ = eglCreateContext(egl_display_, egl_config_, EGL_NO_CONTEXT,
                                  display_context_attributes);
  if (egl_context_ == EGL_NO_CONTEXT) {
    LogEglError("Failed to create EGL context");
    return false;
  }

  egl_resource_context_ = eglCreateContext(
      egl_display_, egl_config_, egl_context_, display_context_attributes);

  if (egl_resource_context_ == EGL_NO_CONTEXT) {
    LogEglError("Failed to create EGL resource context");
    return false;
  }

  return true;
}

void AngleSurfaceManager::CleanUp() {
  EGLBoolean result = EGL_FALSE;

  // Needs to be reset before destroying the EGLContext.
  resolved_device_.Reset();

  if (egl_display_ != EGL_NO_DISPLAY && egl_context_ != EGL_NO_CONTEXT) {
    result = eglDestroyContext(egl_display_, egl_context_);
    egl_context_ = EGL_NO_CONTEXT;

    if (result == EGL_FALSE) {
      LogEglError("Failed to destroy context");
    }
  }

  if (egl_display_ != EGL_NO_DISPLAY &&
      egl_resource_context_ != EGL_NO_CONTEXT) {
    result = eglDestroyContext(egl_display_, egl_resource_context_);
    egl_resource_context_ = EGL_NO_CONTEXT;

    if (result == EGL_FALSE) {
      LogEglError("Failed to destroy resource context");
    }
  }

  if (egl_display_ != EGL_NO_DISPLAY) {
    // Display is reused between instances so only terminate display
    // if destroying last instance
    if (instance_count_ == 1) {
      eglTerminate(egl_display_);
    }
    egl_display_ = EGL_NO_DISPLAY;
  }
}

bool AngleSurfaceManager::CreateSurface(WindowsRenderTarget* render_target,
                                        EGLint width,
                                        EGLint height,
                                        bool vsync_enabled) {
  if (!initialize_succeeded_) {
    return false;
  }

  EGLSurface surface = EGL_NO_SURFACE;

  if (!render_target) {
    RenderTargetDxgiSharedHandle* dxgi_target = new RenderTargetDxgiSharedHandle(width, height);
    
    Microsoft::WRL::ComPtr<ID3D11Device> d3d11_device;
    if (!GetDevice(d3d11_device.ReleaseAndGetAddressOf())) {
      return false;
    }

    if (!dxgi_target->Initialize(d3d11_device)) {
      return false;
    }
    render_target_.reset(dxgi_target);
    void* shared_handle = dxgi_target->shared_handle();
    void* texture = dxgi_target->texture();

    EGLint numConfigs = 0;
    EGLint configAttrs[] = {
        EGL_RENDERABLE_TYPE,
        EGL_OPENGL_ES3_BIT,  // must remain in this position for ES2 fallback
        EGL_SURFACE_TYPE,
        EGL_PBUFFER_BIT,
        // EGL_BUFFER_SIZE,
        // 32,
        EGL_RED_SIZE,
        8,
        EGL_GREEN_SIZE,
        8,
        EGL_BLUE_SIZE,
        8,
        EGL_ALPHA_SIZE,
        8,
        // EGL_DEPTH_SIZE,
        // 0,
        // EGL_STENCIL_SIZE,
        // 0,
        EGL_DEPTH_SIZE, 8, EGL_STENCIL_SIZE, 8,
        // EGL_SAMPLE_BUFFERS,
        // 0,
        EGL_NONE};

    EGLConfig config = nullptr;
    if (eglChooseConfig(egl_display_, configAttrs, &config, 1, &numConfigs) !=
        EGL_TRUE) {
      return false;
    }

    // EGLSurface surface = EGL_NO_SURFACE;
    EGLint surfAttrs[] = {EGL_WIDTH,
                          width,
                          EGL_HEIGHT,
                          height,
                          EGL_TEXTURE_TARGET,
                          EGL_TEXTURE_2D,
                          EGL_TEXTURE_FORMAT,
                          EGL_TEXTURE_RGBA,
                          EGL_NONE};

    surface = eglCreatePbufferFromClientBuffer(egl_display_, EGL_D3D_TEXTURE_ANGLE,
                                              texture, config, surfAttrs);
    if (surface == EGL_NO_SURFACE) {
      // fallback to ES2 - it could be that we're running on older hardware
      // and ES3 isn't available

      // EGL_RENDERABLE_TYPE is the bit at configAttrs[0]
      configAttrs[1] = EGL_OPENGL_ES2_BIT;
      config = nullptr;
      if (eglChooseConfig(egl_display_, configAttrs, &config, 1, &numConfigs) ==
          EGL_TRUE) {
        surface = eglCreatePbufferFromClientBuffer(
            egl_display_, EGL_D3D_TEXTURE_ANGLE, texture, config, surfAttrs);
      }

      // still no surface? we're done
      if (surface == EGL_NO_SURFACE) {
        return false;
      }
    }

    dxgi_target->SetSurface(surface, 0);

    EGLSurface drawSurface = eglGetCurrentSurface(EGL_DRAW);
    EGLSurface readSurface = eglGetCurrentSurface(EGL_READ);

    eglMakeCurrent(egl_display_, surface, surface, egl_context_);

    if (eglBindTexImage(egl_display_, surface, EGL_BACK_BUFFER)) {
      // if (tex_man) {
      //   TextureRef* texture_ref = tex_man->GetTexture(texture_id);
      //   tex_man->SetLevelInfo(texture_ref, GL_TEXTURE_2D, 0, GL_BGRA_EXT, width,
      //                         height, 1, 0, GL_BGRA_EXT, GL_UNSIGNED_BYTE,
      //                         gfx::Rect(size));
      //   tex_man->SetLevelImage(texture_ref, GL_TEXTURE_2D, 0, image.get(),
      //                         Texture::BOUND);
      // }
    }

    eglMakeCurrent(egl_display_, drawSurface, readSurface, egl_context_);
  } else {
    const EGLint surfaceAttributes[] = {
        EGL_FIXED_SIZE_ANGLE, EGL_TRUE, EGL_WIDTH, width,
        EGL_HEIGHT,           height,   EGL_NONE};

    surface = eglCreateWindowSurface(
        egl_display_, egl_config_,
        static_cast<EGLNativeWindowType>(std::get<HWND>(*render_target)),
        surfaceAttributes);
    if (surface == EGL_NO_SURFACE) {
      LogEglError("Surface creation failed.");
      return false;
    }
  }

  surface_width_ = width;
  surface_height_ = height;
  render_surface_ = surface;

  SetVSyncEnabled(vsync_enabled);
  return true;
}

void AngleSurfaceManager::ResizeSurface(WindowsRenderTarget* render_target,
                                        EGLint width,
                                        EGLint height,
                                        bool vsync_enabled) {
  EGLint existing_width, existing_height;
  GetSurfaceDimensions(&existing_width, &existing_height);
  if (width != existing_width || height != existing_height) {
    surface_width_ = width;
    surface_height_ = height;

    ClearContext();
    DestroySurface();
    if (!CreateSurface(nullptr, width, height, vsync_enabled)) {
      FML_LOG(ERROR)
          << "AngleSurfaceManager::ResizeSurface failed to create surface";
    }
  }
}

void AngleSurfaceManager::GetSurfaceDimensions(EGLint* width, EGLint* height) {
  if (render_surface_ == EGL_NO_SURFACE || !initialize_succeeded_) {
    *width = 0;
    *height = 0;
    return;
  }

  // Can't use eglQuerySurface here; Because we're not using
  // EGL_FIXED_SIZE_ANGLE flag anymore, Angle may resize the surface before
  // Flutter asks it to, which breaks resize redraw synchronization
  *width = surface_width_;
  *height = surface_height_;
}

void AngleSurfaceManager::DestroySurface() {
  if (egl_display_ != EGL_NO_DISPLAY && render_surface_ != EGL_NO_SURFACE) {
    eglDestroySurface(egl_display_, render_surface_);
  }
  render_surface_ = EGL_NO_SURFACE;
  render_target_.reset();
}

bool AngleSurfaceManager::MakeCurrent() {
  return (eglMakeCurrent(egl_display_, render_surface_, render_surface_,
                         egl_context_) == EGL_TRUE);
}

bool AngleSurfaceManager::ClearContext() {
  return (eglMakeCurrent(egl_display_, nullptr, nullptr, egl_context_) ==
          EGL_TRUE);
}

bool AngleSurfaceManager::MakeResourceCurrent() {
  return (eglMakeCurrent(egl_display_, EGL_NO_SURFACE, EGL_NO_SURFACE,
                         egl_resource_context_) == EGL_TRUE);
}

EGLBoolean AngleSurfaceManager::SwapBuffers() {
  EGLBoolean result = eglSwapBuffers(egl_display_, render_surface_);
  if (render_target_) {
    RenderTargetDxgiSharedHandle* dxgi_target = 
        reinterpret_cast<RenderTargetDxgiSharedHandle*>(render_target_.get());
    if (dxgi_target) {
      dxgi_target->Unlock();
      void* shared_handle = dxgi_target->shared_handle();
      int width = dxgi_target->width();
      int height = dxgi_target->height();
      if (accelerated_paint_callback_) {
        accelerated_paint_callback_(shared_handle, width, height);
      }
    }
  }
  return (result);
}

EGLSurface AngleSurfaceManager::CreateSurfaceFromHandle(
    EGLenum handle_type,
    EGLClientBuffer handle,
    const EGLint* attributes) const {
  return eglCreatePbufferFromClientBuffer(egl_display_, handle_type, handle,
                                          egl_config_, attributes);
}

void AngleSurfaceManager::SetVSyncEnabled(bool enabled) {
  return;
  if (eglMakeCurrent(egl_display_, render_surface_, render_surface_,
                     egl_context_) != EGL_TRUE) {
    LogEglError("Unable to make surface current to update the swap interval");
    return;
  }

  // OpenGL swap intervals can be used to prevent screen tearing.
  // If enabled, the raster thread blocks until the v-blank.
  // This is unnecessary if DWM composition is enabled.
  // See: https://www.khronos.org/opengl/wiki/Swap_Interval
  // See: https://learn.microsoft.com/windows/win32/dwm/composition-ovw
  if (eglSwapInterval(egl_display_, enabled ? 1 : 0) != EGL_TRUE) {
    LogEglError("Unable to update the swap interval");
    return;
  }
}

bool AngleSurfaceManager::GetDevice(ID3D11Device** device) {
  if (!resolved_device_) {
    PFNEGLQUERYDISPLAYATTRIBEXTPROC egl_query_display_attrib_EXT =
        reinterpret_cast<PFNEGLQUERYDISPLAYATTRIBEXTPROC>(
            eglGetProcAddress("eglQueryDisplayAttribEXT"));

    PFNEGLQUERYDEVICEATTRIBEXTPROC egl_query_device_attrib_EXT =
        reinterpret_cast<PFNEGLQUERYDEVICEATTRIBEXTPROC>(
            eglGetProcAddress("eglQueryDeviceAttribEXT"));

    if (!egl_query_display_attrib_EXT || !egl_query_device_attrib_EXT) {
      return false;
    }

    EGLAttrib egl_device = 0;
    EGLAttrib angle_device = 0;
    if (egl_query_display_attrib_EXT(egl_display_, EGL_DEVICE_EXT,
                                     &egl_device) == EGL_TRUE) {
      if (egl_query_device_attrib_EXT(
              reinterpret_cast<EGLDeviceEXT>(egl_device),
              EGL_D3D11_DEVICE_ANGLE, &angle_device) == EGL_TRUE) {
        resolved_device_ = reinterpret_cast<ID3D11Device*>(angle_device);
      }
    }
  }

  resolved_device_.CopyTo(device);
  return (resolved_device_ != nullptr);
}

}  // namespace flutter

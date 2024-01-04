
#include "include/flutter/flutter_view.h"

#include <algorithm>
#include <iostream>

namespace flutter {

void FlutterView::SetPaintCallback(std::function<void(void*, int, int)> callback) {
  paint_callback_ = std::move(callback);
  FlutterDesktopEngineSetPaintCallback(
      view_,
      [](void* buffer, int width, int height, void* user_data){
        FlutterView* self = static_cast<FlutterView*>(user_data);
        self->paint_callback_(buffer, width, height);
      }, 
      this);
}

void FlutterView::SetAcceleratedPaintCallback(std::function<void(void*, int, int)> callback) {
  accelerated_paint_callback_ = std::move(callback);
  FlutterDesktopEngineSetAcceleratedPaintCallback(
      view_,
      [](void* shared_handle, int width, int height, void* user_data){
        FlutterView* self = static_cast<FlutterView*>(user_data);
        self->accelerated_paint_callback_(shared_handle, width, height);
      }, 
      this);
}

}

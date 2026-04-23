#pragma once
#include "../../core/Handle.h"

// GPU 资源句柄 — 仅在 backend 内部流通
using TextureHandle = core::Handle<struct TextureTag>;
using ShaderHandle  = core::Handle<struct ShaderTag>;
using BufferHandle  = core::Handle<struct BufferTag>;
using SoundHandle   = core::Handle<struct SoundTag>;

#include "CommandBuffer.h"
#include "../../core/Assert.h"
#include <algorithm>

namespace backend {

void CommandBuffer::begin() {
    ASSERT_MSG(!recording_, "CommandBuffer::begin() called twice");
    recording_ = true;
    cmds_.clear();
}

void CommandBuffer::end() {
    ASSERT_MSG(recording_, "CommandBuffer::end() without begin()");
    recording_ = false;
}

void CommandBuffer::reset() {
    cmds_.clear();
    recording_ = false;
}

void CommandBuffer::clear(const core::Color& color) {
    ASSERT_MSG(recording_, "CommandBuffer not recording");
    cmds_.emplace_back(ClearCmd{color});
}

void CommandBuffer::setCamera(const CameraData& cam, engine::RenderPass pass) {
    ASSERT_MSG(recording_, "CommandBuffer not recording");
    cmds_.emplace_back(SetCameraCmd{cam, pass});
}

void CommandBuffer::drawSprite(const DrawSpriteCmd& cmd) {
    ASSERT_MSG(recording_, "CommandBuffer not recording");
    cmds_.emplace_back(cmd);
}

void CommandBuffer::drawTile(const DrawTileCmd& cmd) {
    ASSERT_MSG(recording_, "CommandBuffer not recording");
    cmds_.emplace_back(cmd);
}


} // namespace backend
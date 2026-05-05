#include <cstdio>

#include <engine/framework/NativeModAPI.h>

namespace {

void touchFile(const char* path) {
    FILE* f = std::fopen(path, "wb");
    if (f) {
        std::fputs("ok\n", f);
        std::fclose(f);
    }
}

} // namespace

QGAME_MOD_EXPORT bool qgame_mod_init(engine::QGameModContext* ctx) {
    if (!ctx || ctx->apiVersion != engine::QGAME_MOD_API_VERSION) {
        return false;
    }
    if (ctx->log && ctx->log->info) {
        ctx->log->info("framework smoke native init");
    }
    touchFile("framework_smoke_native_init.txt");
    return true;
}

QGAME_MOD_EXPORT void qgame_mod_shutdown(engine::QGameModContext* ctx) {
    if (ctx && ctx->log && ctx->log->info) {
        ctx->log->info("framework smoke native shutdown");
    }
    touchFile("framework_smoke_native_shutdown.txt");
}

#include <cstdio>

#include <engine/framework/NativeModAPI.h>

namespace {

int gNativeAsset = 7;

void touchFile(const char* path) {
    FILE* f = std::fopen(path, "wb");
    if (f) {
        std::fputs("ok\n", f);
        std::fclose(f);
    }
}

void systemUpdate(void*, float) {
    touchFile("framework_smoke_native_system_update.txt");
}

void systemShutdown(void*) {
    touchFile("framework_smoke_native_system_shutdown.txt");
}

void onSmokeEvent(void*, const char*, const char*) {
    touchFile("framework_smoke_native_event.txt");
}

void* loadSmokeAsset(void*, const char*, const char*) {
    touchFile("framework_smoke_native_loader.txt");
    return &gNativeAsset;
}

} // namespace

QGAME_MOD_EXPORT bool qgame_mod_init(engine::QGameModContext* ctx) {
    if (!ctx || ctx->apiVersion != engine::QGAME_MOD_API_VERSION) {
        return false;
    }
    if (ctx->log && ctx->log->info) {
        ctx->log->info("framework smoke native init");
    }
    if (!ctx->systems || !ctx->systems->register_system ||
        !ctx->events || !ctx->events->subscribe ||
        !ctx->assetLoaders || !ctx->assetLoaders->register_loader) {
        return false;
    }

    engine::QGameNativeSystemDesc system{};
    system.id = "native_smoke.system";
    system.update = systemUpdate;
    system.shutdown = systemShutdown;
    if (!ctx->systems->register_system(ctx, &system)) {
        return false;
    }
    if (!ctx->events->subscribe(ctx, "native.smoke", nullptr, onSmokeEvent)) {
        return false;
    }
    if (!ctx->assetLoaders->register_loader(ctx, "native.smoke.asset",
                                            nullptr, loadSmokeAsset, nullptr)) {
        return false;
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

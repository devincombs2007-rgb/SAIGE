// The prop table against the real assets: every model's engine-space bounds, as the engine's own importer reports them, must match the table's `visual` box. The collision hulls are built from
// that table, so this is what keeps a drawn prop and its hull the same shape. Skipped (exit 77) when the assets are not fetched: pass the asset directory as argv[1] or set SAIGE_HORIZON_ASSETS.
#include "props.hpp"

#include "saige/gfx_assets/gltf.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>

using namespace saige::horizon;

int main(int argc, char** argv)
{
    const char* env = std::getenv("SAIGE_HORIZON_ASSETS");
    const std::filesystem::path root = argc > 1 ? argv[1] : env != nullptr ? env : "";
    if (root.empty() || !std::filesystem::exists(root / "models")) {
        std::printf("assets not available (pass the asset directory or set SAIGE_HORIZON_ASSETS): skipped\n");
        return 77;
    }
    int failures = 0, checked = 0;
    for (std::uint8_t i = 0; i < static_cast<std::uint8_t>(PropType::Count); ++i) {
        const PropType type = static_cast<PropType>(i);
        const PropSpec& spec = prop_spec(type);
        if (spec.model == nullptr) continue;
        const auto path = root / "models" / spec.model / (std::string(spec.model) + ".gltf");
        const auto loaded = saige::gfx::load_gltf_file(path);
        if (!loaded.ok()) {
            std::fprintf(stderr, "FAIL: %s does not load: %s\n", spec.model, loaded.error.c_str());
            ++failures;
            continue;
        }
        const saige::gfx::Aabb& b = loaded.asset->bounds;
        const float want[6] = {spec.visual.min_x, spec.visual.min_y, spec.visual.min_z, spec.visual.max_x, spec.visual.max_y, spec.visual.max_z};
        const float got[6] = {b.min.x, b.min.y, b.min.z, b.max.x, b.max.y, b.max.z};
        for (int k = 0; k < 6; ++k) {
            if (std::fabs(want[k] - got[k]) > 0.02f) {
                std::fprintf(stderr, "FAIL: %s bound %d is %.3f in the table but %.3f in the file\n", spec.model, k, static_cast<double>(want[k]), static_cast<double>(got[k]));
                ++failures;
            }
        }
        if (spec.solid) {
            // the hull lies inside the model's bounds (a hull larger than its model would be an invisible wall)
            const float hull[6] = {spec.hull.min_x, spec.hull.min_y, spec.hull.min_z, spec.hull.max_x, spec.hull.max_y, spec.hull.max_z};
            for (int k = 0; k < 6; ++k) {
                const bool is_min = k < 3;
                if ((is_min && hull[k] < want[k] - 0.02f) || (!is_min && hull[k] > want[k] + 0.02f)) {
                    std::fprintf(stderr, "FAIL: %s hull side %d sticks out of the model\n", spec.model, k);
                    ++failures;
                }
            }
        }
        ++checked;
    }
    std::printf("%d prop models checked against the importer, %d failure(s)\n", checked, failures);
    return failures == 0 ? 0 : 1;
}

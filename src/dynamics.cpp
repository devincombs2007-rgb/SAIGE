#include "saige/dynamics.hpp"

namespace saige {

#ifdef SAIGE_HAVE_JOLT
std::unique_ptr<DynamicsWorld> make_jolt_dynamics_world(const DynamicsConfig& config);  // engine/physics_jolt/jolt_dynamics.cpp
#endif

std::vector<DynamicsBackend> available_dynamics_backends() {
    std::vector<DynamicsBackend> out;
#ifdef SAIGE_HAVE_JOLT
    out.push_back(DynamicsBackend::Jolt);
#endif
    return out;
}

std::unique_ptr<DynamicsWorld> make_dynamics_world(DynamicsBackend backend, const DynamicsConfig& config) {
    switch (backend) {
    case DynamicsBackend::Jolt:
#ifdef SAIGE_HAVE_JOLT
        return make_jolt_dynamics_world(config);
#else
        (void)config;
        return nullptr;
#endif
    }
    return nullptr;
}

} // namespace saige

# SAIGE World Editor — Build 012

**Coordinate Convention:** Right-handed, Z-up, meters (`64.0 m` chunks).  
**Build & Test (C++20):**
```bash
SRCS="source/src/editor_scene.cpp source/src/editor_commands.cpp source/src/editor_project.cpp source/src/editor_workspace.cpp source/src/editor_assets.cpp source/src/editor_semantic.cpp source/src/editor_materials.cpp"
g++ -std=c++20 -O2 -DNDEBUG -Wall -Wextra -Wpedantic -Werror -Wconversion -Isource/include $SRCS tests/editor_core_test.cpp -o ed12_core
./ed12_core
g++ -std=c++20 -O2 -DNDEBUG -Wall -Wextra -Wpedantic -Werror -Wconversion -Isource/include $SRCS tests/editor_build012_test.cpp -o ed12_new
./ed12_new
```

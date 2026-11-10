/* miniz for the title's unpack mode, with forever-unpack.h's settings. The
 * vendored code keeps its own warnings; the title build uses -Werror. NDEBUG
 * keeps miniz's asserts from adding an __assert import. */
#pragma clang diagnostic ignored "-Weverything"
#define NDEBUG 1
#include "forever-unpack.h"
#include "../../addon-worker/third_party/miniz/miniz.c"
#include "../../addon-worker/third_party/miniz/miniz_tinfl.c"
#include "../../addon-worker/third_party/miniz/miniz_zip.c"

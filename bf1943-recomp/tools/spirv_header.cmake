# Wraps glslc -mfmt=num output (comma-separated uint32 words) into a C++ array.
file(READ ${IN} words)
file(WRITE ${OUT} "#pragma once\n#include <cstdint>\nalignas(4) static const uint32_t ${VAR}[] = {\n${words}};\n")

// The one file that compiles toml++'s implementation; everything else includes it with
// TOML_HEADER_ONLY=0 (set on sf_core), which keeps builds fast. Same setup as star-forge/patcher.
#define TOML_IMPLEMENTATION
#include <toml.hpp>

Import("env")
import os

project_dir = env.subst("$PROJECT_DIR")
build_dir = env.subst("$BUILD_DIR")
ft8_lib = os.path.join(project_dir, "lib", "ft8_lib")

# Add include directories for ft8_lib headers
env.Append(CPPPATH=[
    ft8_lib,
    os.path.join(ft8_lib, "ft8_decode"),
    os.path.join(ft8_lib, "common"),
    os.path.join(ft8_lib, "fft"),
])

# Build only decoder-specific files that don't already exist in src/ft8/
# (constants.c, crc.c, encode.c, text.c are already in src/ft8/ — skip them)
env.BuildSources(
    os.path.join(build_dir, "ft8_lib_sources"),
    ft8_lib,
    src_filter=(
        "-<*> "
        "+<ft8_decode/decode.c> "
        "+<ft8_decode/decoder_api.c> "
        "+<ft8_decode/ldpc.c> "
        "+<ft8_decode/pack.c> "
        "+<ft8_decode/unpack.c> "
        "+<src/decode_ft8.c> "
        "+<common/wave.c>"
    )
)

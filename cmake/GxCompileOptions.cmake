# Project-wide compiler settings. Every first-party target calls gx_configure_target().
#
# Floating point: deterministic simulation forbids value-changing optimizations. MSVC /fp:precise does not
# reassociate or contract to FMA (since VS 2022 contraction needs an explicit /fp:contract); GCC/Clang need
# -ffp-contract=off because they contract by default on targets with FMA.

function(gx_configure_target target)
    target_compile_features(${target} PUBLIC cxx_std_20)

    if(MSVC)
        target_compile_options(${target} PRIVATE
            /W4
            /permissive-
            /utf-8
            /Zc:__cplusplus
            /Zc:preprocessor
            /EHsc
            /fp:precise
            /wd4324  # structure padded due to alignment specifier: intentional for cache-line alignment
        )
        target_compile_definitions(${target} PRIVATE _CRT_SECURE_NO_WARNINGS)
        if(GX_WARNINGS_AS_ERRORS)
            target_compile_options(${target} PRIVATE /WX)
        endif()
    else()
        target_compile_options(${target} PRIVATE
            -Wall
            -Wextra
            -Wpedantic
            -ffp-contract=off
            -fno-fast-math
        )
        if(GX_WARNINGS_AS_ERRORS)
            target_compile_options(${target} PRIVATE -Werror)
        endif()
    endif()
endfunction()

# Build metadata for executables that report results (benchmarks, headless runs).
function(gx_add_build_info target)
    target_compile_definitions(${target} PRIVATE
        GX_BUILD_CONFIG="$<CONFIG>"
        GX_VERSION="${PROJECT_VERSION}"
    )
endfunction()

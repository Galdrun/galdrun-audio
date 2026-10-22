# Warning flags applied per target. Nothing is set globally, so a consuming
# build or a vendored dependency is never forced into our flag set.

function(multiroom_set_warnings target)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /permissive- /utf-8)
        # fopen/gmtime and friends are used deliberately; the "secure" variants
        # are not portable.
        target_compile_definitions(${target} PRIVATE _CRT_SECURE_NO_WARNINGS)
    else()
        target_compile_options(${target} PRIVATE
            -Wall
            -Wextra
            -Wpedantic
            -Wshadow
            -Wnon-virtual-dtor
        )
    endif()
endfunction()

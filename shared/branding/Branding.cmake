# The product's brand for the C++ builds (client, core, ESP32). Code
# identifiers use the fixed codename "multiroom" instead; only what a user or
# another program sees is derived from these two values.
set(MULTIROOM_BRAND_NAME "Galdrun")  # display: page titles, setup SSID
set(MULTIROOM_BRAND_SLUG "galdrun")  # contracts: exe names, paths, ids

# Hands both to a target's sources as string-literal macros.
function(multiroom_brand target scope)
    target_compile_definitions(${target} ${scope}
        MULTIROOM_BRAND_NAME="${MULTIROOM_BRAND_NAME}"
        MULTIROOM_BRAND_SLUG="${MULTIROOM_BRAND_SLUG}")
endfunction()

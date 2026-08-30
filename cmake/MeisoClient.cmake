# The client facade owns public validation and dispatch only. Backends install
# their private operations into each opaque handle, so neither the Host nor
# Simulation implementation leaks into the other's link closure.

add_library(
    meiso_composition_edge
    integrations/composition/composition_edge.c
)
meiso_configure_public_target(meiso_composition_edge CompositionEdge)
target_include_directories(
    meiso_composition_edge
    PRIVATE integrations/composition
)
target_link_libraries(
    meiso_composition_edge
    PUBLIC meiso_composition_profile
)
meiso_enable_warnings(meiso_composition_edge)
meiso_set_target_folder(
    meiso_composition_edge
    "Meiso/Integrations/Client"
)
add_library(Meiso::CompositionEdge ALIAS meiso_composition_edge)

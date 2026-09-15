# Runtime owns catalog, route, transfer and scheduling state. It consumes the
# portable Core contracts but does not depend on a Profile or platform adapter.

add_library(meiso_runtime
    src/runtime/meiso_runtime.c
    src/runtime/meiso_runtime_wire.c
    src/runtime/meiso_runtime_state_codec.c
    src/runtime/meiso_runtime_catalog.c
    src/runtime/meiso_runtime_catalog_receive.c
    src/runtime/meiso_runtime_route.c
    src/runtime/meiso_runtime_state.c
    src/runtime/meiso_runtime_stream.c
    src/runtime/meiso_runtime_blob_transfer.c
    src/runtime/meiso_runtime_blob_store.c
    src/runtime/meiso_runtime_receive.c
    src/runtime/meiso_runtime_query.c
)
meiso_configure_public_target(meiso_runtime Runtime)
target_link_libraries(meiso_runtime PUBLIC meiso_core)
meiso_enable_warnings(meiso_runtime)
meiso_set_target_folder(meiso_runtime "Meiso/Runtime")
add_library(Meiso::Runtime ALIAS meiso_runtime)

if(MEISO_BUILD_LEGACY_COMPOSITION)
    add_library(meiso_runtime_scheduler
        src/runtime/meiso_runtime_scheduler.c
        src/runtime/meiso_runtime_scheduler_boundary.c
        src/runtime/meiso_runtime_scheduler_work.c
        src/runtime/meiso_runtime_scheduler_step.c
    )
    meiso_configure_public_target(meiso_runtime_scheduler RuntimeScheduler)
    target_link_libraries(meiso_runtime_scheduler PUBLIC meiso_runtime)
    meiso_enable_warnings(meiso_runtime_scheduler)
    meiso_set_target_folder(meiso_runtime_scheduler "Meiso/Runtime")
    add_library(Meiso::RuntimeScheduler ALIAS meiso_runtime_scheduler)
endif()

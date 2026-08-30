# Test targets are opt-in through CTest's BUILD_TESTING option. Modules are
# loaded in registration order so test identities and dashboard ordering remain
# stable while each lane owns a bounded target graph.

include("${CMAKE_CURRENT_LIST_DIR}/tests/Support.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/tests/WebInstall.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/tests/CoreRuntime.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/tests/ProfilesRenderers.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/tests/IntegrationServices.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/tests/TransportServices.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/tests/TargetPolicyConformance.cmake")

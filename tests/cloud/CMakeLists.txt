# Cloud library tests (0.7, docs/cloud.md): storage layer, locks and cloud projects, review links and comments, stock media. Fake
# servers stand in for S3, WebDAV and the stock services; nothing reaches the network. Outputs land in <build>/test-output/cloud/.
# The model suite's timeline fixture builds the small projects the sync tests edit.
find_package(nlohmann_json CONFIG REQUIRED)
wabcut_add_test_suite(cloud
    SOURCES
        FakeServers.h
        ${CMAKE_SOURCE_DIR}/tests/model/TimelineFixture.cpp
        ${CMAKE_SOURCE_DIR}/tests/model/TimelineFixture.h
        test_cloud_projects.cpp
        test_cloud_review.cpp
        test_cloud_stores.cpp
        test_stock.cpp
    LIBS wabcut_cloud wabcut_model wabcut_ai)
target_include_directories(wabcut_cloud_tests PRIVATE ${CMAKE_SOURCE_DIR}/tests/model)
target_link_libraries(wabcut_cloud_tests PRIVATE nlohmann_json::nlohmann_json)
target_compile_definitions(wabcut_cloud_tests PRIVATE WABCUT_TEST_MEDIA_DIR="${CMAKE_BINARY_DIR}/test-output/media")

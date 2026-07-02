cmake_minimum_required(VERSION 3.24)

project(SC2DataHelper VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

find_package(Qt6 6.5 REQUIRED COMPONENTS Widgets Test Concurrent)

set(SPDLOG_BUILD_EXAMPLE OFF CACHE BOOL "" FORCE)
set(SPDLOG_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(SPDLOG_BUILD_BENCH OFF CACHE BOOL "" FORCE)
set(SPDLOG_INSTALL OFF CACHE BOOL "" FORCE)
add_subdirectory(third_party/spdlog EXCLUDE_FROM_ALL)

set(PUGIXML_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(PUGIXML_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(PUGIXML_INSTALL OFF CACHE BOOL "" FORCE)
add_subdirectory(third_party/pugixml EXCLUDE_FROM_ALL)

set(STORM_SKIP_INSTALL ON CACHE BOOL "" FORCE)
set(STORM_USE_BUNDLED_LIBRARIES ON CACHE BOOL "" FORCE)
set(STORM_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(STORM_UNICODE ON CACHE BOOL "" FORCE)
add_subdirectory(third_party/StormLib EXCLUDE_FROM_ALL)

add_library(nlohmann_json INTERFACE)
add_library(nlohmann_json::nlohmann_json ALIAS nlohmann_json)
target_include_directories(nlohmann_json INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/nlohmann_json/include
)

qt_standard_project_setup()

add_executable(SC2DataHelper WIN32)

target_sources(SC2DataHelper PRIVATE
    src/main.cpp
    src/app/MainWindow.h
    src/app/MainWindow.cpp
    src/app/MainWindowAnalysisController.h
    src/app/MainWindowAnalysisController.cpp
    src/app/MainWindowSettings.h
    src/app/MainWindowSettings.cpp
    src/app/MainWindowStartup.h
    src/app/MainWindowStartup.cpp
    src/app/MainWindowUiBuilder.h
    src/app/MainWindowUiBuilder.cpp
    src/app/MainWindowUiSupport.h
    src/app/MainWindowUiSupport.cpp
    src/app/ModalBackdrop.h
    src/app/ModalBackdrop.cpp
    src/app/OptimizationWizardController.h
    src/app/OptimizationWizardController.cpp
    src/app/Sc2FileDialogs.h
    src/app/Sc2FileDialogs.cpp
    src/app/Sc2MessageDialog.h
    src/app/Sc2MessageDialog.cpp
    src/app/SourceSelectionController.h
    src/app/SourceSelectionController.cpp
    src/app/AudioManager.h
    src/app/AudioManager.cpp
    src/app/ThemeManager.h
    src/app/ThemeManager.cpp
    src/core/Sc2Archive.h
    src/core/Sc2Archive.cpp
    src/core/XmlLoader.h
    src/core/XmlLoader.cpp
    src/core/DataNode.h
    src/core/CatalogProtection.h
    src/core/CatalogEnumRepair.h
    src/core/CatalogEnumRepair.cpp
    src/core/CatalogLinkSchema.h
    src/core/CatalogLinkSchema.cpp
    src/core/ArchiveReferenceRewriter.h
    src/core/ArchiveReferenceRewriter.cpp
    src/core/AnalysisModels.h
    src/core/AnalysisModels.cpp
    src/core/FolderAnalyzer.h
    src/core/FolderAnalyzer.cpp
    src/core/ReferenceGraph.h
    src/core/ReferenceGraph.cpp
    src/core/DuplicateFinder.h
    src/core/DuplicateFinder.cpp
    src/core/UnusedFinder.h
    src/core/UnusedFinder.cpp
    src/core/XmlFormatter.h
    src/core/XmlFormatter.cpp
    src/core/BackupManager.h
    src/core/BackupManager.cpp
    src/core/MergeService.h
    src/core/MergeService.cpp
    src/core/UnitFamilyDetector.h
    src/core/UnitFamilyDetector.cpp
    src/core/StandardNamePlanner.h
    src/core/StandardNamePlanner.cpp
    src/core/ReferenceRenamer.h
    src/core/ReferenceRenamer.cpp
    src/core/DataCollectionAliasMapper.h
    src/core/DataCollectionAliasMapper.cpp
    src/core/DataCollectionPreservation.h
    src/core/DataCollectionPreservation.cpp
    src/core/DataCollectionUnitBuilder.h
    src/core/DataCollectionUnitBuilder.cpp
    src/core/DeepCleanupService.h
    src/core/DeepCleanupService.cpp
    src/core/ConfigManager.h
    src/core/ConfigManager.cpp
    src/ui/OverviewPage.h
    src/ui/OverviewPage.cpp
    src/ui/AnalysisProgressDialog.h
    src/ui/AnalysisProgressDialog.cpp
    src/ui/ObjectInspectorWidget.h
    src/ui/ObjectInspectorWidget.cpp
    src/ui/DependenciesPage.h
    src/ui/DependenciesPage.cpp
    src/ui/GraphPage.h
    src/ui/GraphPage.cpp
    src/ui/PropertiesPage.h
    src/ui/PropertiesPage.cpp
    src/ui/DataCollectionPage.h
    src/ui/DataCollectionPage.cpp
    src/ui/RenameIdsPage.h
    src/ui/RenameIdsPage.cpp
    src/ui/UnusedPage.h
    src/ui/UnusedPage.cpp
    src/ui/DuplicatesPage.h
    src/ui/DuplicatesPage.cpp
    src/ui/FormatterPage.h
    src/ui/FormatterPage.cpp
    src/ui/LogPanel.h
    src/ui/LogPanel.cpp
    src/ui/XmlSourcePage.h
    src/ui/XmlSourcePage.cpp
    src/ui/ObjectTableModel.h
    src/ui/ObjectTableModel.cpp
    src/ui/ObjectFilterProxyModel.h
    src/ui/ObjectFilterProxyModel.cpp
    resources/resources.qrc
)

if (WIN32)
    target_sources(SC2DataHelper PRIVATE resources/app_icon.rc)
endif()

target_include_directories(SC2DataHelper PRIVATE src)

target_link_libraries(SC2DataHelper PRIVATE
    Qt6::Widgets
    Qt6::Concurrent
    spdlog::spdlog_header_only
    pugixml
    StormLib::storm
    nlohmann_json::nlohmann_json
)

target_compile_definitions(SC2DataHelper PRIVATE SC2DH_USE_STORMLIB=1 UNICODE _UNICODE)

if (WIN32)
    target_link_libraries(SC2DataHelper PRIVATE winmm)
endif()

set(SC2DH_HAVE_LIBZIP OFF)
if (TARGET libzip::zip)
    target_link_libraries(SC2DataHelper PRIVATE libzip::zip)
    target_compile_definitions(SC2DataHelper PRIVATE SC2DH_USE_LIBZIP=1)
    set(SC2DH_HAVE_LIBZIP ON)
else()
    find_path(LIBZIP_INCLUDE_DIR zip.h)
    find_library(LIBZIP_LIBRARY NAMES zip libzip)
    if (LIBZIP_INCLUDE_DIR AND LIBZIP_LIBRARY)
        target_include_directories(SC2DataHelper PRIVATE ${LIBZIP_INCLUDE_DIR})
        target_link_libraries(SC2DataHelper PRIVATE ${LIBZIP_LIBRARY})
        target_compile_definitions(SC2DataHelper PRIVATE SC2DH_USE_LIBZIP=1)
        set(SC2DH_HAVE_LIBZIP ON)
    endif()
endif()

set(SC2DH_RUNTIME_RESOURCES_DIR ${CMAKE_CURRENT_BINARY_DIR}/resources)
set(SC2DH_RUNTIME_SCRIPTS_DIR ${CMAKE_CURRENT_BINARY_DIR}/scripts)
file(MAKE_DIRECTORY "${SC2DH_RUNTIME_RESOURCES_DIR}/styles")
file(MAKE_DIRECTORY "${SC2DH_RUNTIME_RESOURCES_DIR}/config")
file(MAKE_DIRECTORY "${SC2DH_RUNTIME_SCRIPTS_DIR}")

configure_file(resources/styles/dark.qss "${SC2DH_RUNTIME_RESOURCES_DIR}/styles/dark.qss" COPYONLY)
configure_file(resources/catalog_link_schema.tsv "${SC2DH_RUNTIME_RESOURCES_DIR}/catalog_link_schema.tsv" COPYONLY)
configure_file(resources/catalogsData.xsd "${SC2DH_RUNTIME_RESOURCES_DIR}/catalogsData.xsd" COPYONLY)
configure_file(resources/config/rules.json "${SC2DH_RUNTIME_RESOURCES_DIR}/config/rules.json" COPYONLY)
configure_file(resources/config/whitelist.json "${SC2DH_RUNTIME_RESOURCES_DIR}/config/whitelist.json" COPYONLY)
configure_file(scripts/validate_sc2_catalogs.py "${SC2DH_RUNTIME_SCRIPTS_DIR}/validate_sc2_catalogs.py" COPYONLY)

install(TARGETS SC2DataHelper)

enable_testing()

add_executable(SC2DataHelperTests
    tests/test_core.cpp
    src/core/AnalysisModels.cpp
    src/core/CatalogEnumRepair.cpp
    src/core/CatalogLinkSchema.cpp
    src/core/ArchiveReferenceRewriter.cpp
    src/core/FolderAnalyzer.cpp
    src/core/BackupManager.cpp
    src/core/Sc2Archive.cpp
    src/core/XmlLoader.cpp
    src/core/DuplicateFinder.cpp
    src/core/MergeService.cpp
    src/core/UnitFamilyDetector.cpp
    src/core/StandardNamePlanner.cpp
    src/core/ReferenceRenamer.cpp
    src/core/DataCollectionAliasMapper.cpp
    src/core/DataCollectionPreservation.cpp
    src/core/DataCollectionUnitBuilder.cpp
    src/core/DeepCleanupService.cpp
    src/ui/ObjectTableModel.cpp
    src/ui/ObjectFilterProxyModel.cpp
)

target_include_directories(SC2DataHelperTests PRIVATE src)
target_link_libraries(SC2DataHelperTests PRIVATE
    Qt6::Test
    Qt6::Widgets
    pugixml
    StormLib::storm
    nlohmann_json::nlohmann_json
)

target_compile_definitions(SC2DataHelperTests PRIVATE SC2DH_USE_STORMLIB=1 UNICODE _UNICODE)

if (SC2DH_HAVE_LIBZIP)
    target_compile_definitions(SC2DataHelperTests PRIVATE SC2DH_USE_LIBZIP=1)
    if (TARGET libzip::zip)
        target_link_libraries(SC2DataHelperTests PRIVATE libzip::zip)
    else()
        target_link_libraries(SC2DataHelperTests PRIVATE ${LIBZIP_LIBRARY})
        target_include_directories(SC2DataHelperTests PRIVATE ${LIBZIP_INCLUDE_DIR})
    endif()
endif()

add_test(NAME SC2DataHelperTests COMMAND SC2DataHelperTests)

add_executable(SC2OptimizeFolder
    src/tools/OptimizeFolder.cpp
    src/core/AnalysisModels.cpp
    src/core/CatalogLinkSchema.cpp
    src/core/ArchiveReferenceRewriter.cpp
    src/core/FolderAnalyzer.cpp
    src/core/BackupManager.cpp
    src/core/Sc2Archive.cpp
    src/core/CatalogEnumRepair.cpp
    src/core/XmlLoader.cpp
    src/core/DuplicateFinder.cpp
    src/core/MergeService.cpp
    src/core/DeepCleanupService.cpp
    src/core/ConfigManager.cpp
)
target_include_directories(SC2OptimizeFolder PRIVATE src)
target_link_libraries(SC2OptimizeFolder PRIVATE
    Qt6::Widgets
    pugixml
    StormLib::storm
    nlohmann_json::nlohmann_json
)
target_compile_definitions(SC2OptimizeFolder PRIVATE SC2DH_USE_STORMLIB=1 UNICODE _UNICODE)

if (SC2DH_HAVE_LIBZIP)
    target_compile_definitions(SC2OptimizeFolder PRIVATE SC2DH_USE_LIBZIP=1)
    if (TARGET libzip::zip)
        target_link_libraries(SC2OptimizeFolder PRIVATE libzip::zip)
    else()
        target_link_libraries(SC2OptimizeFolder PRIVATE ${LIBZIP_LIBRARY})
        target_include_directories(SC2OptimizeFolder PRIVATE ${LIBZIP_INCLUDE_DIR})
    endif()
endif()

add_executable(SC2DataCollectionProbe
    src/tools/DataCollectionProbe.cpp
    src/core/AnalysisModels.cpp
    src/core/CatalogLinkSchema.cpp
    src/core/ArchiveReferenceRewriter.cpp
    src/core/FolderAnalyzer.cpp
    src/core/BackupManager.cpp
    src/core/Sc2Archive.cpp
    src/core/CatalogEnumRepair.cpp
    src/core/XmlLoader.cpp
    src/core/DuplicateFinder.cpp
    src/core/MergeService.cpp
    src/core/UnitFamilyDetector.cpp
    src/core/StandardNamePlanner.cpp
    src/core/ReferenceRenamer.cpp
    src/core/DataCollectionAliasMapper.cpp
    src/core/DataCollectionPreservation.cpp
    src/core/DataCollectionUnitBuilder.cpp
    src/core/DeepCleanupService.cpp
)
target_include_directories(SC2DataCollectionProbe PRIVATE src)
target_link_libraries(SC2DataCollectionProbe PRIVATE
    Qt6::Widgets
    pugixml
    StormLib::storm
    nlohmann_json::nlohmann_json
)
target_compile_definitions(SC2DataCollectionProbe PRIVATE SC2DH_USE_STORMLIB=1 UNICODE _UNICODE)

if (SC2DH_HAVE_LIBZIP)
    target_compile_definitions(SC2DataCollectionProbe PRIVATE SC2DH_USE_LIBZIP=1)
    if (TARGET libzip::zip)
        target_link_libraries(SC2DataCollectionProbe PRIVATE libzip::zip)
    else()
        target_link_libraries(SC2DataCollectionProbe PRIVATE ${LIBZIP_LIBRARY})
        target_include_directories(SC2DataCollectionProbe PRIVATE ${LIBZIP_INCLUDE_DIR})
    endif()
endif()

add_executable(SC2OptimizeMaps
    src/tools/OptimizeMaps.cpp
    src/core/AnalysisModels.cpp
    src/core/CatalogLinkSchema.cpp
    src/core/ArchiveReferenceRewriter.cpp
    src/core/FolderAnalyzer.cpp
    src/core/BackupManager.cpp
    src/core/Sc2Archive.cpp
    src/core/CatalogEnumRepair.cpp
    src/core/XmlLoader.cpp
    src/core/DuplicateFinder.cpp
    src/core/MergeService.cpp
    src/core/UnitFamilyDetector.cpp
    src/core/StandardNamePlanner.cpp
    src/core/ReferenceRenamer.cpp
    src/core/DataCollectionAliasMapper.cpp
    src/core/DataCollectionPreservation.cpp
    src/core/DataCollectionUnitBuilder.cpp
    src/core/DeepCleanupService.cpp
)
target_include_directories(SC2OptimizeMaps PRIVATE src)
target_link_libraries(SC2OptimizeMaps PRIVATE
    Qt6::Widgets
    pugixml
    StormLib::storm
    nlohmann_json::nlohmann_json
)
target_compile_definitions(SC2OptimizeMaps PRIVATE SC2DH_USE_STORMLIB=1 UNICODE _UNICODE)

if (SC2DH_HAVE_LIBZIP)
    target_compile_definitions(SC2OptimizeMaps PRIVATE SC2DH_USE_LIBZIP=1)
    if (TARGET libzip::zip)
        target_link_libraries(SC2OptimizeMaps PRIVATE libzip::zip)
    else()
        target_link_libraries(SC2OptimizeMaps PRIVATE ${LIBZIP_LIBRARY})
        target_include_directories(SC2OptimizeMaps PRIVATE ${LIBZIP_INCLUDE_DIR})
    endif()
endif()

add_executable(SC2RepairMapEnums
    src/tools/RepairMapEnums.cpp
    src/core/CatalogEnumRepair.cpp
    src/core/Sc2Archive.cpp
)
target_include_directories(SC2RepairMapEnums PRIVATE src)
target_link_libraries(SC2RepairMapEnums PRIVATE
    Qt6::Core
    pugixml
    StormLib::storm
    nlohmann_json::nlohmann_json
)
target_compile_definitions(SC2RepairMapEnums PRIVATE SC2DH_USE_STORMLIB=1 UNICODE _UNICODE)

if (SC2DH_HAVE_LIBZIP)
    target_compile_definitions(SC2RepairMapEnums PRIVATE SC2DH_USE_LIBZIP=1)
    if (TARGET libzip::zip)
        target_link_libraries(SC2RepairMapEnums PRIVATE libzip::zip)
    else()
        target_link_libraries(SC2RepairMapEnums PRIVATE ${LIBZIP_LIBRARY})
        target_include_directories(SC2RepairMapEnums PRIVATE ${LIBZIP_INCLUDE_DIR})
    endif()
endif()

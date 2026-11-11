set(KRITA_SUBTOOLDOCKER_SOURCES
    SubToolDockerPlugin.cpp
    SubToolDock.cpp
    SubToolDelegate.cpp
    SubToolGroups.cpp
)
kis_add_library(kritasubtooldocker MODULE ${KRITA_SUBTOOLDOCKER_SOURCES})
target_link_libraries(kritasubtooldocker kritaui kritaresourcewidgets)
install(TARGETS kritasubtooldocker DESTINATION ${KRITA_PLUGIN_INSTALL_DIR})

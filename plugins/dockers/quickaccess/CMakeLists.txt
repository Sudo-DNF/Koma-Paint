set(KRITA_QUICKACCESS_SOURCES
    QuickAccessPlugin.cpp
    QuickAccessStore.cpp
    QuickAccessGrid.cpp
    QuickAccessDock.cpp
)
kis_add_library(kritaquickaccess MODULE ${KRITA_QUICKACCESS_SOURCES})
target_link_libraries(kritaquickaccess kritaui)
install(TARGETS kritaquickaccess DESTINATION ${KRITA_PLUGIN_INSTALL_DIR})
install(FILES quickaccess.action DESTINATION ${KDE_INSTALL_DATADIR}/krita/actions)

set(kritacorrectline_SOURCES
    KomaCorrectLinePlugin.cpp
    KomaCorrectLineTool.cpp
)
kis_add_library(kritacorrectline MODULE ${kritacorrectline_SOURCES})
target_link_libraries(kritacorrectline kritaui)
install(TARGETS kritacorrectline DESTINATION ${KRITA_PLUGIN_INSTALL_DIR})
install(FILES correctline.action DESTINATION ${KDE_INSTALL_DATADIR}/krita/actions)

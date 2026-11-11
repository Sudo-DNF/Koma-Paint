set(KRITA_MIXINGPALETTE_SOURCES
    MixingPalettePlugin.cpp
    MixingPaletteDock.cpp
    MixingPaletteCanvas.cpp
)
kis_add_library(kritamixingpalette MODULE ${KRITA_MIXINGPALETTE_SOURCES})
target_link_libraries(kritamixingpalette kritaui)
install(TARGETS kritamixingpalette DESTINATION ${KRITA_PLUGIN_INSTALL_DIR})

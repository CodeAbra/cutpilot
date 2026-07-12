qt_add_library(cutpilot-render STATIC
    include/cutpilot/render/CanvasController.h
    include/cutpilot/render/CanvasItem.h
    include/cutpilot/render/CompositorEngine.h
    include/cutpilot/render/NodeLayerItem.h
    src/CanvasController.cpp
    src/CanvasItem.cpp
    src/CanvasRenderer.cpp
    src/CompositorEngine.cpp
    src/NodeLayerItem.cpp
    src/NodeGeometryBuilder.cpp
    src/NodeContentRasterizer.cpp
    src/ConnectorGeometryBuilder.cpp)

target_include_directories(cutpilot-render
    PUBLIC include
    PRIVATE src)

target_link_libraries(cutpilot-render
    PUBLIC
        cutpilot-core
        cutpilot-theme
        Qt6::Quick
    PRIVATE
        Qt6::GuiPrivate)

# Bake the grid and compositor shaders to .qsb and embed them under the
# resource prefix the renderers load from.
qt_add_shaders(cutpilot-render "cutpilot_render_shaders"
    PREFIX "/cutpilot/render/shaders"
    BASE "${CMAKE_CURRENT_SOURCE_DIR}/shaders"
    FILES
        shaders/grid.vert
        shaders/grid.frag
        shaders/composite.vert
        shaders/blend.frag
        shaders/mask.frag
        shaders/key.frag
        shaders/transform.frag)

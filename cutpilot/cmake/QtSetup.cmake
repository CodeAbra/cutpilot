# Locates the Qt 6 modules the platform depends on.
#
# The GPU canvas renders through QRhi, a semi-private Qt API reached via the
# GuiPrivate component: it carries no source or binary compatibility guarantee
# across Qt minor releases, so the version is pinned exactly rather than floored.
# QQuickRhiItem, which the grid layer subclasses, also requires 6.7 or newer.
# ShaderTools provides qt_add_shaders to bake .qsb shader packs at build time.

set(CUTPILOT_QT_VERSION 6.11.1 CACHE STRING "Exact Qt version the renderer is pinned to")

find_package(Qt6 ${CUTPILOT_QT_VERSION} EXACT REQUIRED COMPONENTS
    Core
    Gui
    GuiPrivate
    Quick
    Widgets
    QuickWidgets
    ShaderTools
    Network
    Concurrent
    Multimedia
    Test)

qt_standard_project_setup()

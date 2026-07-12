# Locates the Qt 6 modules the platform depends on.
#
# The GPU canvas renders through QRhi, which is a semi-private Qt API: it is
# reached via the GuiPrivate component and pinned to one Qt version. ShaderTools
# provides qt_add_shaders to bake .qsb shader packs at build time.

find_package(Qt6 6.6 REQUIRED COMPONENTS
    Core
    Gui
    GuiPrivate
    Quick
    Widgets
    QuickWidgets
    ShaderTools)

qt_standard_project_setup()

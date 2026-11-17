#!/bin/sh
# Builds build/exp/<name>: copies of the loose QML with one part of Main.qml removed (bench.ps1 -Set parts).
set -e
cd "$(dirname "$0")"
rm -rf build/exp
mk() { mkdir -p build/exp/$1; cp qml/*.qml build/exp/$1/; cp qml/loose.qmldir build/exp/$1/qmldir; }
mk full
mk no_menubar;   sed -i '/menuBar: AppMenuBar/d' build/exp/no_menubar/Main.qml
mk no_palette;   sed -i '/CommandPalette {/d; /Shortcut {/d' build/exp/no_palette/Main.qml
mk no_ribbon;    sed -i '/header: MainToolBar/d' build/exp/no_ribbon/Main.qml
mk no_props;     sed -i '/PropertiesPanel {/,/}/d' build/exp/no_props/Main.qml
mk no_sidepanel; sed -i '/SidePanelTabs {/,/}/d' build/exp/no_sidepanel/Main.qml
mk no_timeline;  sed -i '/TimelineBar {/d' build/exp/no_timeline/Main.qml
mk no_start;     sed -i '/StartScreen {/d' build/exp/no_start/Main.qml
mk viewport_only
sed -i '/menuBar: AppMenuBar/d; /header: MainToolBar/d; /footer: StatusBar/d; /CommandPalette {/d; /Shortcut {/d; /PropertiesPanel {/,/}/d; /SidePanelTabs {/,/}/d; /TimelineBar {/d; /StartScreen {/d; /Toast {/d' build/exp/viewport_only/Main.qml

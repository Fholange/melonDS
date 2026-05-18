#ifndef SETTINGS_H
#define SETTINGS_H

#include "BoxGui.h"

namespace SettingsDialog
{

void DoGui(BoxGui::Frame& parent);

// Shared sync status label + button — usable from any menu
void DoSyncWidget(BoxGui::Frame& parent, BoxGui::Skewer& skewer);

}

#endif
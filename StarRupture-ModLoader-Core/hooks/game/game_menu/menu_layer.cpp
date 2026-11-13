#include "pch.h"

#ifdef MODLOADER_CLIENT_BUILD

// The menu-layer read behind the plugins' menu bits (UIState). It has a file
// of its own because the layout table tags the classes read here with the
// ui-menus consumer (tools\Update-LayoutTable.ps1): when one of them changes
// on a game build, only this read is switched off (UIState::DisableMenuRead),
// not the plugins.

#include "game_menu.h"
#include "core/ui_state.h"
#include "logging/logger.h"

#include "ChimeraUI_classes.hpp"
#include "CommonGame_classes.hpp"
#include "CommonUI_classes.hpp"

#include <string>
#include <vector>

namespace Hooks::GameMenu
{
	namespace
	{
		// The layer UCrUIManagerSubsystem opens the map, the inventory, the
		// build menu and every building window on; the HUD pushes its escape
		// menu there too. The HUD itself sits on another layer.
		constexpr const char* kMenuLayerTag = "UI.Layer.Menu";

		// The tag's FName comparison index once a layer has matched it by
		// string, so later ticks compare an int. 0 until then.
		int32_t g_menuLayerTagIndex = 0;

		bool IsMenuLayerTag(const SDK::FName& tag)
		{
			if (g_menuLayerTagIndex != 0)
				return tag.ComparisonIndex == g_menuLayerTagIndex && tag.Number == 0;
			if (tag.ToString() != kMenuLayerTag)
				return false;
			g_menuLayerTagIndex = tag.ComparisonIndex;
			return true;
		}

		// Calls visit(tag, stack) for each layer of the local player's root
		// layout, until it returns true. Valid for the current tick only.
		template <typename Visit>
		void VisitLayers(SDK::APlayerController* pc, Visit&& visit)
		{
			auto* ui = static_cast<SDK::UGameUIManagerSubsystem*>(
				SDK::USubsystemBlueprintLibrary::GetGameInstanceSubsystem(pc, SDK::UCrUIManagerSubsystem::StaticClass()));
			SDK::UGameUIPolicy* policy = ui ? ui->CurrentPolicy : nullptr;
			if (!policy)
				return;

			for (int32_t i = 0; i < policy->RootViewportLayouts.Num(); ++i)
			{
				const SDK::FRootViewportLayoutInfo& info = policy->RootViewportLayouts[i];
				if (!info.RootLayout || info.LocalPlayer != pc->Player)
					continue;

				// The class is named so the layout table lists it and the
				// check covers Layers. Not const: the SDK's const TMap indexer
				// does not compile.
				SDK::UPrimaryGameLayout* root = info.RootLayout;
				auto& layers = root->Layers;
				for (int32_t l = 0; l < layers.NumAllocated(); ++l)
				{
					if (!layers.IsValidIndex(l))
						continue;
					SDK::UCommonActivatableWidgetContainerBase* stack = layers[l].Value();
					if (stack && visit(layers[l].Key().TagName, stack))
						return;
				}
			}
		}

		// The local player's menu layer, or null. Valid for the current tick only.
		SDK::UCommonActivatableWidgetContainerBase* FindMenuLayer(SDK::APlayerController* pc)
		{
			SDK::UCommonActivatableWidgetContainerBase* menuLayer = nullptr;
			VisitLayers(pc, [&](const SDK::FName& tag, SDK::UCommonActivatableWidgetContainerBase* stack)
			{
				if (!IsMenuLayerTag(tag))
					return false;
				menuLayer = stack;
				return true;
			});
			return menuLayer;
		}

		// One entry of what LogLayerChanges compares from tick to tick: a layer
		// (widgetClass 0, objectIndex = its widget count) or a widget on it.
		// Plain values only; nothing here is dereferenced on a later tick.
		struct LayerEntry
		{
			int32_t   layerTag;     // the layer tag's FName comparison index
			uintptr_t widgetClass;  // compared as a number, never followed
			int32_t   objectIndex;  // the widget's GObjects index
			bool      active;       // bIsActive
			uint8_t   visibility;   // ESlateVisibility
			bool      displayed;    // the stack's DisplayedWidget

			bool operator==(const LayerEntry& o) const
			{
				return layerTag == o.layerTag && widgetClass == o.widgetClass && objectIndex == o.objectIndex
					&& active == o.active && visibility == o.visibility && displayed == o.displayed;
			}
		};

		const char* VisibilityName(SDK::ESlateVisibility v)
		{
			switch (v)
			{
			case SDK::ESlateVisibility::Visible:              return "Visible";
			case SDK::ESlateVisibility::Collapsed:            return "Collapsed";
			case SDK::ESlateVisibility::Hidden:               return "Hidden";
			case SDK::ESlateVisibility::HitTestInvisible:     return "HitTestInvisible";
			case SDK::ESlateVisibility::SelfHitTestInvisible: return "SelfHitTestInvisible";
			default:                                          return "?";
			}
		}
	}

	uint64_t ReadMenuLayer(SDK::APlayerController* pc, bool* buildMenuActive)
	{
		*buildMenuActive = false;
		if (!pc)
			return 0;
		SDK::UCommonActivatableWidgetContainerBase* layer = FindMenuLayer(pc);
		if (!layer)
			return 0;

		// Every widget on the stack's list counts, closing ones too: a widget
		// stays on WidgetList until its close transition has ended, long after
		// it stopped being the active one (bIsActive). So the bits stay set
		// from the moment a screen is pushed until it is fully gone.
		constexpr int32_t kMaxKinds = 16;
		UIState::MenuKind kinds[kMaxKinds];
		const int32_t     count = layer->WidgetList.Num();
		const int32_t     kept  = count < kMaxKinds ? count : kMaxKinds;
		for (int32_t i = 0; i < kept; ++i)
		{
			SDK::UCommonActivatableWidget* w = layer->WidgetList[i];
			if (!w)
				kinds[i] = UIState::MenuKind::Other;
			else if (w->IsA(SDK::UCrUW_MapMenu::StaticClass()))
				kinds[i] = UIState::MenuKind::Map;
			else if (w->IsA(SDK::UCrUW_InventoryScreen::StaticClass()))
				kinds[i] = UIState::MenuKind::Inventory;
			else if (w->IsA(SDK::UCrUW_BuildingMenu::StaticClass()))
			{
				kinds[i] = UIState::MenuKind::BuildMenu;
				if (w->bIsActive)
					*buildMenuActive = true;
			}
			else if (w->IsA(SDK::UCrUW_ResearchTerminalPanel::StaticClass())
			      || w->IsA(SDK::UCrUW_UpgradeStationPanel::StaticClass())
			      || w->IsA(SDK::UCrUW_ArmoryPanel::StaticClass()))
				kinds[i] = UIState::MenuKind::StationMenu;
			else
				kinds[i] = UIState::MenuKind::Other;
		}

		return UIState::MenuBits(static_cast<size_t>(count), kinds, static_cast<size_t>(kept));
	}

	bool ReadMenuHover(SDK::APlayerController* pc)
	{
		// Without a menu layer to read, or with nothing on it while a cursor
		// screen is up (the screen is on some other layer), the loader cannot
		// tell where the menu's widgets are: true, so a widget stays click-through.
		if (!pc)
			return true;
		SDK::UCommonActivatableWidgetContainerBase* layer = FindMenuLayer(pc);
		if (!layer || layer->WidgetList.Num() == 0)
			return true;

		// A widget is hovered only while the cursor is over a hit-testable
		// part of it, so a hidden or hit-test-invisible screen reads false.
		// Slate hovers the whole path down to the widget under the cursor, so
		// a screen's root counts as soon as one of its buttons or panels does.
		constexpr int32_t kMaxWidgets = 16;
		const int32_t     count       = layer->WidgetList.Num();
		const int32_t     read        = count < kMaxWidgets ? count : kMaxWidgets;
		for (int32_t i = 0; i < read; ++i)
		{
			SDK::UCommonActivatableWidget* w = layer->WidgetList[i];
			if (w && w->IsHovered())
				return true;
		}
		return false;
	}

	void LogLayerChanges(SDK::APlayerController* pc, uint64_t uiState, bool cursorFlag, bool menuReadRan)
	{
		// Game thread only, so plain statics. A run that changes every tick
		// stops logging after kMaxLines.
		constexpr int32_t kMaxWidgets = 16;
		constexpr int     kMaxLines   = 300;
		static std::vector<LayerEntry> s_last;
		static std::vector<LayerEntry> s_now;
		static uint64_t s_lastState  = ~0ull;
		static bool     s_lastCursor = false;
		static bool     s_lastRead   = false;
		static int      s_lines      = 0;
		if (s_lines > kMaxLines)
			return;

		s_now.clear();
		VisitLayers(pc, [&](const SDK::FName& tag, SDK::UCommonActivatableWidgetContainerBase* stack)
		{
			const int32_t count = stack->WidgetList.Num();
			s_now.push_back({ tag.ComparisonIndex, 0, count, false, 0, false });
			const int32_t read = count < kMaxWidgets ? count : kMaxWidgets;
			for (int32_t i = 0; i < read; ++i)
			{
				SDK::UCommonActivatableWidget* w = stack->WidgetList[i];
				if (!w)
					continue;
				s_now.push_back({ tag.ComparisonIndex, reinterpret_cast<uintptr_t>(w->Class), w->Index, w->bIsActive,
				                  static_cast<uint8_t>(w->GetVisibility()), w == stack->DisplayedWidget });
			}
			return false;
		});

		if (s_now == s_last && uiState == s_lastState && cursorFlag == s_lastCursor && menuReadRan == s_lastRead)
			return;
		s_last.swap(s_now);
		s_lastState  = uiState;
		s_lastCursor = cursorFlag;
		s_lastRead   = menuReadRan;

		if (++s_lines > kMaxLines)
		{
			ModLoaderLogger::LogInfo(L"[UI] Layer changes: more than %d, no longer logged", kMaxLines);
			return;
		}

		ModLoaderLogger::LogInfo(
			L"[UI] Layers changed: state 0x%llX (any menu %d, build menu %d, cursor up %d), "
			L"the game's cursor flag %hs, menu read %hs",
			static_cast<unsigned long long>(uiState),
			(uiState & PLUGIN_UI_ANY_MENU) ? 1 : 0, (uiState & PLUGIN_UI_BUILD_MENU) ? 1 : 0,
			(uiState & PLUGIN_UI_CURSOR_UP) ? 1 : 0, cursorFlag ? "on" : "off", menuReadRan ? "on" : "off");

		// The names are read now, from this tick's objects.
		VisitLayers(pc, [&](const SDK::FName& tag, SDK::UCommonActivatableWidgetContainerBase* stack)
		{
			const int32_t count = stack->WidgetList.Num();
			std::string line;
			const int32_t read = count < kMaxWidgets ? count : kMaxWidgets;
			for (int32_t i = 0; i < read; ++i)
			{
				SDK::UCommonActivatableWidget* w = stack->WidgetList[i];
				line += i ? "; " : "";
				if (!w)
				{
					line += "(null)";
					continue;
				}
				line += w->Class ? w->Class->GetName() : std::string("?");
				if (w->IsA(SDK::UCrUW_BuildingMenu::StaticClass()))
					line += " [build menu]";
				line += w->bIsActive ? " active " : " inactive ";
				line += VisibilityName(w->GetVisibility());
				if (w == stack->DisplayedWidget)
					line += " displayed";
			}
			ModLoaderLogger::LogInfo(L"[UI]   %hs: %d widget(s)%hs%hs", tag.ToString().c_str(), count,
			                         count ? ": " : "", line.c_str());
			return false;
		});
	}
}

#endif // MODLOADER_CLIENT_BUILD

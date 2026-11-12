#pragma once

// The loader's engine side of the image lookups (image_lookup.h): the item
// list the asset registry gives and the icon of each item, and the object
// path of a live texture. Client builds only. Game thread only.

#include "UI/image_lookup.h"

namespace SDK { class UTexture2D; }

namespace ImageLookup
{
	// The loader's item source, for the pump.
	ItemSource& EngineItems();

	// The lookup behind IPluginImages::FromTexture.
	PluginImageId FromTexture(SDK::UTexture2D* texture);
}

#include "UI/image_lookup.h"

#include <algorithm>

namespace ImageLookup
{
	bool IsPlaceholderImage(const char* texturePath)
	{
		// The key is an object path in lower case, so the asset is what follows the last dot.
		static const char* const kPlaceholders[] = { "whitesquaretexture", "defaultwhitegrid", "whiteplaceholder" };

		const std::string key = ImageLibrary::NormalizeObjectPath(texturePath);
		const size_t dot = key.rfind('.');
		if (dot == std::string::npos)
			return false;
		const std::string asset = key.substr(dot + 1);
		return std::any_of(std::begin(kPlaceholders), std::end(kPlaceholders),
			[&](const char* placeholder) { return asset == placeholder; });
	}

	// ---- Items -----------------------------------------------------------------

	ItemIndex::ItemIndex(ItemSource& source)
		: m_source(source)
	{
	}

	void ItemIndex::Step(uint64_t budgetUs, const std::function<uint64_t()>& nowUs)
	{
		if (m_state == State::Done || m_state == State::Failed)
			return;

		const uint64_t begin = nowUs();
		if (m_state == State::Idle)
		{
			if (!m_source.List())
			{
				m_state = State::Failed;
				return;
			}
			m_state = State::Scanning;
		}

		do
		{
			if (m_next >= m_source.Count())
			{
				m_state = State::Done;
				return;
			}

			ItemRecord record;
			const ItemRead read = m_source.Read(m_next++, record);
			if (read == ItemRead::Unavailable)
			{
				m_state = State::Failed;
				return;
			}
			if (read == ItemRead::Skipped)
				continue;

			// An item with no icon is known all the same: it has no image to show.
			const std::string key = ImageLibrary::NormalizeItemName(record.name.c_str());
			if (!key.empty())
				m_items.emplace(key, read == ItemRead::Icon ? std::move(record.texturePath) : std::string());
		}
		while (nowUs() - begin < budgetUs);

		if (m_next >= m_source.Count())
			m_state = State::Done;
	}

	bool ItemIndex::Find(const std::string& key, std::string& texturePath) const
	{
		const auto it = m_items.find(key);
		if (it == m_items.end())
			return false;
		texturePath = it->second;
		return true;
	}

	// ---- Textures --------------------------------------------------------------

	TextureLookup::TextureLookup(TextureEngine& engine, ImageLibrary::Library& library)
		: m_engine(engine), m_library(library)
	{
	}

	PluginImageId TextureLookup::FromTexture(const void* texture)
	{
		if (!texture)
			return 0;
		if (!m_engine.OnGameThread())
		{
			if (!m_loggedThread)
			{
				m_loggedThread = true;
				m_engine.Log("[Images] FromTexture was called from a thread other than the game's: it gives 0 there");
			}
			return 0;
		}

		TextureSlot slot;
		if (!m_engine.SlotOf(texture, slot))
			return 0;
		const uint64_t key = (static_cast<uint64_t>(static_cast<uint32_t>(slot.index)) << 32) | static_cast<uint32_t>(slot.serial);
		const auto known = m_known.find(key);
		if (known != m_known.end())
			return known->second;

		std::string path;
		if (!m_engine.PathOf(texture, path))
			return 0;
		const PluginImageId id = m_library.Path(path.c_str());
		if (id == 0)
			return 0;

		if (m_known.size() >= kTextureCacheMax)
			m_known.clear();
		m_known.emplace(key, id);
		return id;
	}
}

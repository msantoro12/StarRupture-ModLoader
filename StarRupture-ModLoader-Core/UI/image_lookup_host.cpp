#include "pch.h"
#include "UI/image_lookup_host.h"

#ifdef MODLOADER_CLIENT_BUILD

#include "hooks/game/gobject_walk/gobject_walk.h"
#include "hooks/game/object_lookup/object_lookup.h"
#include "hooks/game/object_properties/object_properties.h"
#include "logging/log.h"
#include "utils/game_thread_dispatch.h"
#include "Engine_classes.hpp"
#include "Engine_parameters.hpp"
#include "AuItems_classes.hpp"
#include "AssetRegistry_classes.hpp"
#include "AssetRegistry_parameters.hpp"

#include <initializer_list>
#include <utility>
#include <vector>

// Nothing here is compiled for one game build. Fields are found by name in the
// game's own reflection data. The engine functions that run as script calls
// (the asset registry, the path of an object) are checked against the game's
// own parameter block first, and not used where they differ. A pointer into the
// engine lives inside one call.
namespace ImageLookup
{
	namespace
	{
		namespace OP = Hooks::ObjectProperties;

		using FindObject_t  = SDK::UObject*  (__fastcall*)(SDK::UClass*, SDK::UObject*, const wchar_t*, bool);
		using FindPackage_t = SDK::UPackage* (__fastcall*)(SDK::UObject*, const wchar_t*);
		using FullyLoad_t   = void           (__fastcall*)(SDK::UPackage*);
		using LoadPackage_t = SDK::UPackage* (__fastcall*)(SDK::UPackage*, const wchar_t*, uint32_t, void*, const void*);

		// FUNC_Native. A script call is made only on a function that has it
		// already; the flag is never written, since its offset is not confirmed
		// on every build.
		constexpr uint32_t kFuncNative = 0x400;

		// A bad pointer inside the engine reads as an access violation or a page
		// error. Anything else (a breakpoint, a stack overflow) is not ours to swallow.
		int SehFilter(DWORD code)
		{
			return (code == EXCEPTION_ACCESS_VIOLATION || code == EXCEPTION_IN_PAGE_ERROR)
				? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH;
		}

		std::wstring Widen(const std::string& text)
		{
			if (text.empty())
				return {};
			const int length = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), nullptr, 0);
			if (length <= 0)
				return {};
			std::wstring wide(static_cast<size_t>(length), L'\0');
			MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), wide.data(), length);
			return wide;
		}

		// ---- Engine calls. SEH only: nothing in these owns an object that needs unwinding.

		bool FindSEH(FindObject_t fn, SDK::UClass* cls, SDK::UObject* outer, const wchar_t* path, SDK::UObject** out)
		{
			__try
			{
				*out = fn(cls, outer, path, false);
				return true;
			}
			__except (SehFilter(GetExceptionCode()))
			{
				*out = nullptr;
				return false;
			}
		}

		bool FindPackageSEH(FindPackage_t fn, const wchar_t* path, SDK::UPackage** out)
		{
			__try
			{
				*out = fn(nullptr, path);
				return true;
			}
			__except (SehFilter(GetExceptionCode()))
			{
				*out = nullptr;
				return false;
			}
		}

		bool FullyLoadSEH(FullyLoad_t fn, SDK::UPackage* package)
		{
			__try
			{
				fn(package);
				return true;
			}
			__except (SehFilter(GetExceptionCode()))
			{
				return false;
			}
		}

		bool LoadPackageSEH(LoadPackage_t fn, const wchar_t* path, SDK::UPackage** out)
		{
			__try
			{
				*out = fn(nullptr, path, 0, nullptr, nullptr);
				return true;
			}
			__except (SehFilter(GetExceptionCode()))
			{
				*out = nullptr;
				return false;
			}
		}

		bool InvokeSEH(SDK::UObject* object, SDK::UFunction* function, void* params)
		{
			__try
			{
				object->ProcessEvent(function, params);
				return true;
			}
			__except (SehFilter(GetExceptionCode()))
			{
				return false;
			}
		}

		// ---- Finding things by name

		SDK::UObject* FindByName(SDK::UClass* cls, SDK::UObject* outer, const wchar_t* path)
		{
			const FindObject_t find = reinterpret_cast<FindObject_t>(Hooks::ObjectLookup::GetStaticFindObject_ByNameAddress());
			SDK::UObject* object = nullptr;
			if (!find || !FindSEH(find, cls, outer, path, &object))
				return nullptr;
			return object;
		}

		// A script function called on the default object of its native class,
		// which is found by name each time and not kept.
		bool CallStatic(const wchar_t* defaultObjectPath, SDK::UClass* cls, const char* owner, const char* function, void* params)
		{
			SDK::UObject* object = FindByName(nullptr, nullptr, defaultObjectPath);
			SDK::UFunction* func = cls ? cls->GetFunction(owner, function) : nullptr;
			if (!object || !func || !(static_cast<uint32_t>(func->FunctionFlags) & kFuncNative))
				return false;
			return InvokeSEH(object, func, params);
		}

		std::string Hex(size_t value)
		{
			char text[24];
			snprintf(text, sizeof(text), "0x%zX", value);
			return text;
		}

		// The function's parameter block: its size, and where each named member
		// sits, against the SDK's own struct.
		bool ParamsHold(SDK::UClass* cls, const char* owner, const char* function, size_t sdkSize,
			std::initializer_list<std::pair<const char*, size_t>> members, std::string* why)
		{
			const std::string label = std::string(owner) + "::" + function;
			SDK::UFunction* func = cls ? cls->GetFunction(owner, function) : nullptr;
			if (!func)
			{
				*why = label + " is not in the game's reflection data";
				return false;
			}
			if (static_cast<size_t>(func->Size) != sdkSize)
			{
				*why = label + " takes " + Hex(static_cast<size_t>(func->Size)) + " bytes of parameters here, the SDK's is " + Hex(sdkSize);
				return false;
			}
			for (const auto& member : members)
			{
				SDK::FProperty* property = OP::FindPropertyByName(func, member.first);
				if (!property || static_cast<size_t>(property->Offset) != member.second)
				{
					*why = label + " parameter " + member.first + " is at "
						+ (property ? Hex(static_cast<size_t>(property->Offset)) : std::string("(missing)"))
						+ " here, the SDK's is " + Hex(member.second);
					return false;
				}
			}
			return true;
		}

		// The object path of an object ("/Game/UI/T_X.T_X"), from the engine. The
		// SDK's GetFullName builds it from GetName, which drops everything up to
		// the last '/', and no lookup finds that.
		bool PathNameUsable()
		{
			if (!OP::IsReady())
				return false;
			static const bool usable = []
			{
				using namespace SDK::Params;
				std::string why;
				const bool ok = ParamsHold(SDK::UKismetSystemLibrary::StaticClass(), "KismetSystemLibrary", "GetPathName",
					sizeof(KismetSystemLibrary_GetPathName),
					{ { "Object", offsetof(KismetSystemLibrary_GetPathName, Object) },
					  { "ReturnValue", offsetof(KismetSystemLibrary_GetPathName, ReturnValue) } }, &why);
				if (!ok)
					LogToFile::Warn("[Images] The engine's path of an object cannot be used here: %s", why.c_str());
				return ok;
			}();
			return usable;
		}

		bool PathNameOf(SDK::UObject* object, std::string* out)
		{
			if (!PathNameUsable())
				return false;
			SDK::Params::KismetSystemLibrary_GetPathName params{};
			params.Object = object;
			if (!CallStatic(L"/Script/Engine.Default__KismetSystemLibrary", SDK::UKismetSystemLibrary::StaticClass(),
				"KismetSystemLibrary", "GetPathName", &params))
				return false;
			*out = params.ReturnValue.ToString();
			return !out->empty() && (*out)[0] == '/';
		}

		bool PathNameSEH(SDK::UObject* object, std::string* out)
		{
			__try
			{
				return PathNameOf(object, out);
			}
			__except (SehFilter(GetExceptionCode()))
			{
				return false;
			}
		}

		// ---- The item source

		// A property that is a struct, and one member of that struct.
		bool Nested(SDK::UClass* cls, const char* outerName, const char* innerName, SDK::FProperty** outer,
			SDK::FProperty** inner, OP::PropertyKind innerKind, std::string* why)
		{
			*outer = OP::FindPropertyByName(cls, outerName);
			SDK::UStruct* type = *outer ? OP::GetPropertyStructType(*outer) : nullptr;
			if (!type)
			{
				*why = std::string(outerName) + " is not a struct member in the game's reflection data";
				return false;
			}
			*inner = OP::FindPropertyByName(type, innerName);
			if (!*inner || OP::GetPropertyKind(*inner) != innerKind)
			{
				*why = std::string(outerName) + "." + innerName + " is not in the game's reflection data";
				return false;
			}
			return true;
		}

		// What the item index needs of the asset registry.
		bool RegistryUsable(std::string* why)
		{
			using namespace SDK::Params;
			SDK::UClass* registry = SDK::IAssetRegistry::StaticClass();
			return ParamsHold(SDK::UAssetRegistryHelpers::StaticClass(), "AssetRegistryHelpers", "GetAssetRegistry",
					sizeof(AssetRegistryHelpers_GetAssetRegistry),
					{ { "ReturnValue", offsetof(AssetRegistryHelpers_GetAssetRegistry, ReturnValue) } }, why)
				&& ParamsHold(registry, "AssetRegistry", "GetAssetsByClass", sizeof(AssetRegistry_GetAssetsByClass),
					{ { "ClassPathName", offsetof(AssetRegistry_GetAssetsByClass, ClassPathName) },
					  { "OutAssetData", offsetof(AssetRegistry_GetAssetsByClass, OutAssetData) },
					  { "bSearchSubClasses", offsetof(AssetRegistry_GetAssetsByClass, bSearchSubClasses) },
					  { "ReturnValue", offsetof(AssetRegistry_GetAssetsByClass, ReturnValue) } }, why)
				&& ParamsHold(SDK::UKismetSystemLibrary::StaticClass(), "KismetSystemLibrary", "MakeTopLevelAssetPath",
					sizeof(KismetSystemLibrary_MakeTopLevelAssetPath),
					{ { "PackageName", offsetof(KismetSystemLibrary_MakeTopLevelAssetPath, PackageName) },
					  { "AssetName", offsetof(KismetSystemLibrary_MakeTopLevelAssetPath, AssetName) },
					  { "ReturnValue", offsetof(KismetSystemLibrary_MakeTopLevelAssetPath, ReturnValue) } }, why);
		}

		// The asset registry's assets of one blueprint class.
		bool GetAssetsByClass(const wchar_t* scriptPackage, const wchar_t* className, SDK::TArray<SDK::FAssetData>& out)
		{
			SDK::Params::AssetRegistryHelpers_GetAssetRegistry registryParams{};
			if (!CallStatic(L"/Script/AssetRegistry.Default__AssetRegistryHelpers", SDK::UAssetRegistryHelpers::StaticClass(),
				"AssetRegistryHelpers", "GetAssetRegistry", &registryParams))
				return false;
			SDK::UObject* registry = registryParams.ReturnValue.GetObjectRef();
			SDK::UClass* registryClass = SDK::IAssetRegistry::StaticClass();
			SDK::UFunction* func = registry && registryClass ? registryClass->GetFunction("AssetRegistry", "GetAssetsByClass") : nullptr;
			if (!func || !(static_cast<uint32_t>(func->FunctionFlags) & kFuncNative))
				return false;

			SDK::Params::KismetSystemLibrary_MakeTopLevelAssetPath pathParams{};
			pathParams.PackageName = SDK::FString(scriptPackage);
			pathParams.AssetName   = SDK::FString(className);
			if (!CallStatic(L"/Script/Engine.Default__KismetSystemLibrary", SDK::UKismetSystemLibrary::StaticClass(),
				"KismetSystemLibrary", "MakeTopLevelAssetPath", &pathParams))
				return false;

			SDK::Params::AssetRegistry_GetAssetsByClass params{};
			params.ClassPathName     = pathParams.ReturnValue;
			params.bSearchSubClasses = true;
			if (!InvokeSEH(registry, func, &params))
				return false;
			out = std::move(params.OutAssetData);
			return params.ReturnValue;
		}

		class EngineItemSource final : public ItemSource
		{
		public:
			bool List() override
			{
				m_refs.clear();

				std::string why;
				if (!Prepare(&why))
				{
					LogToFile::Warn("[Images] The item list cannot be made here: %s", why.c_str());
					return false;
				}

				SDK::TArray<SDK::FAssetData> assets;
				if (!GetAssetsByClass(L"/Script/AuItems", L"AuItemBlueprint", assets))
				{
					LogToFile::Warn("[Images] The asset registry gave no item list");
					return false;
				}
				for (int i = 0; i < assets.Num(); ++i)
				{
					// FName::ToString keeps only what follows the last '/', so a package name must be read raw.
					Ref ref = { assets[i].PackageName.GetRawString(), assets[i].AssetName.ToString() };
					if (!ref.package.empty() && !ref.asset.empty())
						m_refs.push_back(std::move(ref));
				}
				LogToFile::Info("[Images] Item list: %zu item blueprints", m_refs.size());
				return !m_refs.empty();
			}

			size_t Count() const override { return m_refs.size(); }

			ItemRead Read(size_t index, ItemRecord& out) override
			{
				const FindPackage_t findPackage = reinterpret_cast<FindPackage_t>(Hooks::ObjectLookup::GetFindPackageAddress());
				const FullyLoad_t   fullyLoad   = reinterpret_cast<FullyLoad_t>(Hooks::ObjectLookup::GetUPackage_FullyLoadAddress());
				const LoadPackage_t loadPackage = reinterpret_cast<LoadPackage_t>(Hooks::ObjectLookup::GetLoadPackageAddress());
				if (!findPackage || !fullyLoad || !loadPackage || !Hooks::ObjectLookup::GetStaticFindObject_ByNameAddress())
					return ItemRead::Unavailable;

				const Ref& ref = m_refs[index];
				const std::wstring packagePath = Widen(ref.package);
				const std::wstring defaultName = Widen("Default__" + ref.asset + "_C");
				if (packagePath.empty() || defaultName.empty())
					return ItemRead::Skipped;

				// The package may be in memory already; if it is, make sure it is whole.
				SDK::UPackage* package = nullptr;
				if (!FindPackageSEH(findPackage, packagePath.c_str(), &package))
					return ItemRead::Skipped;
				if (package ? !FullyLoadSEH(fullyLoad, package) : (!LoadPackageSEH(loadPackage, packagePath.c_str(), &package) || !package))
					return ItemRead::Skipped;

				char name[256];
				SDK::UObject* icon = nullptr;
				const Brush brush = ReadItemSEH(package, defaultName.c_str(), name, static_cast<int>(sizeof(name)), &icon);
				if (brush == Brush::Fault)
					LogToFile::Warn("[Images] Exception reading the item %s", ref.asset.c_str());
				if ((brush != Brush::Texture && brush != Brush::Empty) || !name[0])
					return ItemRead::Skipped;

				out.name = name;
				if (brush == Brush::Empty)
					return ItemRead::NoIcon;
				return PathNameSEH(icon, &out.texturePath) ? ItemRead::Icon : ItemRead::Skipped;
			}

		private:
			struct Ref
			{
				std::string package;   // "/Game/..." the item blueprint's package
				std::string asset;     // the blueprint's name
			};

			// What an item's icon brush holds: a texture, or something that is not one.
			enum class Brush { Texture, Empty, Missing, Fault };

			// The fields the index reads, found by name, and the calls it makes checked.
			bool Prepare(std::string* why)
			{
				if (!OP::IsReady())
				{
					*why = "the game's reflection data is not ready";
					return false;
				}
				SDK::UClass* itemClass = SDK::UAuItemDataBase::StaticClass();
				if (!Nested(itemClass, "ItemIcon", "ResourceObject", &m_icon, &m_iconResource, OP::PropertyKind::Object, why))
					return false;
				m_uniqueName = OP::FindPropertyByName(itemClass, "UniqueItemName");
				if (!m_uniqueName || OP::GetPropertyKind(m_uniqueName) != OP::PropertyKind::Name)
				{
					*why = "UAuItemDataBase::UniqueItemName is not in the game's reflection data";
					return false;
				}
				if (!PathNameUsable())
				{
					*why = "the engine's path of an object cannot be used";
					return false;
				}
				return RegistryUsable(why);
			}

			// The unique name and the icon texture of the item whose default
			// object is called defaultName in the package. The texture is
			// good for this call only.
			Brush ReadItemSEH(SDK::UPackage* package, const wchar_t* defaultName, char* name, int nameSize, SDK::UObject** texture) const
			{
				*texture = nullptr;
				name[0] = '\0';
				__try
				{
					SDK::UClass* itemClass = SDK::UAuItemDataBase::StaticClass();
					SDK::UObject* object = FindByName(itemClass, package, defaultName);
					if (!object || !object->IsA(itemClass))
						return Brush::Missing;

					OP::GetNameProperty(object, m_uniqueName, name, nameSize);
					void* brush = OP::GetPropertyRawPtr(object, m_icon);
					SDK::UObject* resource = nullptr;
					if (!OP::GetObjectProperty(brush, m_iconResource, &resource) || !resource)
						return Brush::Empty;
					if (!resource->IsA(SDK::UTexture2D::StaticClass()))
						return Brush::Empty;
					*texture = resource;
					return Brush::Texture;
				}
				__except (SehFilter(GetExceptionCode()))
				{
					*texture = nullptr;
					return Brush::Fault;
				}
			}

			std::vector<Ref> m_refs;
			// Reflection data, not objects: they stay as long as the class.
			SDK::FProperty*  m_uniqueName   = nullptr;   // UAuItemDataBase::UniqueItemName
			SDK::FProperty*  m_icon         = nullptr;   // UAuItemDataBase::ItemIcon
			SDK::FProperty*  m_iconResource = nullptr;   // FSlateBrush::ResourceObject, in it
		};

		// ---- The texture engine

		bool SlotSEH(const void* texture, TextureSlot* out)
		{
			__try
			{
				SDK::UObject* object = const_cast<SDK::UObject*>(static_cast<const SDK::UObject*>(texture));
				const int32_t index = object->Index;
				if (index < 0 || index >= SDK::UObject::GObjects->Num() || SDK::UObject::GObjects->GetByIndex(index) != object)
					return false;
				if (!object->IsA(SDK::UTexture2D::StaticClass()))
					return false;
				SDK::FUObjectItem* item = Hooks::GObjectWalk::GetUObjectItem(index);
				if (!item)
					return false;
				out->index  = index;
				out->serial = Hooks::GObjectWalk::AllocateOrReadSerialNumber(item);
				return true;
			}
			__except (SehFilter(GetExceptionCode()))
			{
				return false;
			}
		}

		class EngineTextures final : public TextureEngine
		{
		public:
			bool OnGameThread() override { return GameThreadDispatch::IsGameThread(); }

			bool SlotOf(const void* texture, TextureSlot& out) override { return SlotSEH(texture, &out); }

			bool PathOf(const void* texture, std::string& out) override
			{
				return PathNameSEH(const_cast<SDK::UObject*>(static_cast<const SDK::UObject*>(texture)), &out);
			}

			void Log(const std::string& line) override { LogToFile::Info("%s", line.c_str()); }
		};
	}

	// Never destroyed: a plugin can still ask at process exit.
	ItemSource& EngineItems()
	{
		static EngineItemSource* const source = new EngineItemSource();
		return *source;
	}

	PluginImageId FromTexture(SDK::UTexture2D* texture)
	{
		static EngineTextures* const engine = new EngineTextures();
		static TextureLookup* const lookup = new TextureLookup(*engine, ImageLibrary::Instance());
		return lookup->FromTexture(texture);
	}
}

#endif // MODLOADER_CLIENT_BUILD

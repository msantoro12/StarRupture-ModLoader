#include "layout_check.h"

#include <cstdarg>
#include <cstdio>

namespace LayoutCheck
{
    namespace
    {
        std::string Format(const char* format, ...)
        {
            char buffer[512];
            va_list args;
            va_start(args, format);
            const int written = vsnprintf(buffer, sizeof(buffer), format, args);
            va_end(args);
            return written < 0 ? std::string() : std::string(buffer);
        }

        const LiveProperty* FindProperty(const LiveStruct& live, const char* name)
        {
            for (const LiveProperty& property : live.properties)
            {
                if (property.name == name)
                    return &property;
            }
            return nullptr;
        }
    }

    Result Compare(const std::vector<TableClass>& table,
        const std::function<const LiveStruct*(const TableClass&)>& find)
    {
        Result result;
        result.classes = static_cast<int>(table.size());

        for (const TableClass& sdk : table)
        {
            // A Blueprint class is only in memory once its asset loads, so its
            // absence at startup says nothing.
            if (sdk.kind == Kind::Blueprint || sdk.kind == Kind::Unknown)
            {
                ++result.deferred;
                continue;
            }

            const LiveStruct* live = find(sdk);
            if (!live)
            {
                ++result.missing;
                result.changedClasses.push_back(sdk.name);
                result.lines.push_back(Format("%s (%s): not found in the game", sdk.name, sdk.path));
                continue;
            }

            ++result.checked;
            const size_t linesBefore = result.lines.size();

            if (live->size != static_cast<int32_t>(sdk.sizeOf))
                result.lines.push_back(Format("%s: size SDK 0x%X, game 0x%X", sdk.name, sdk.sizeOf, live->size));
            // A native UFunction's MinAlignment is 1 whatever its parameters
            // hold, so only classes and structs have an alignment to compare.
            if (sdk.kind != Kind::FunctionParams && sdk.alignOf != 0
                && live->minAlignment != static_cast<int32_t>(sdk.alignOf))
                result.lines.push_back(Format("%s: alignment SDK %u, game %d", sdk.name, sdk.alignOf, live->minAlignment));

            for (const TableRow& row : sdk.rows)
            {
                const LiveProperty* property = FindProperty(*live, row.name);
                if (!property)
                {
                    result.lines.push_back(Format("%s::%s: not found in the game", sdk.name, row.name));
                    continue;
                }

                if (property->offset != static_cast<int32_t>(row.offset))
                    result.lines.push_back(Format("%s::%s: offset SDK 0x%X, game 0x%X",
                        sdk.name, row.name, row.offset, property->offset));
                if (row.size != 0 && property->size != static_cast<int32_t>(row.size))
                    result.lines.push_back(Format("%s::%s: size SDK 0x%X, game 0x%X",
                        sdk.name, row.name, row.size, property->size));

                if (row.boolMask != 0 && !property->isBool)
                    result.lines.push_back(Format("%s::%s: a bool in the SDK, not in the game", sdk.name, row.name));
                else if (row.boolMask != 0 && property->fieldMask != row.boolMask)
                    result.lines.push_back(Format("%s::%s: bool mask SDK 0x%02X, game 0x%02X",
                        sdk.name, row.name, row.boolMask, property->fieldMask));
                else if (row.boolMask == 0 && property->isBool)
                    result.lines.push_back(Format("%s::%s: a bool in the game, not in the SDK", sdk.name, row.name));
            }

            if (result.lines.size() != linesBefore)
            {
                ++result.mismatched;
                result.changedClasses.push_back(sdk.name);
            }
        }

        return result;
    }
}

#if defined(MODLOADER_CLIENT_BUILD)

#include "layout_table.generated.h"
#include "sdk_globals.h"
#include "../logging/logger.h"
#include "CoreUObject_classes.hpp"

#include <windows.h>

#include <chrono>
#include <unordered_map>

static_assert(SrLayoutTable::kFormatVersion == 1, "layout_check.cpp reads layout table format 1");
static_assert(SrLayoutTable::kDetail == 1, "an embedded layout table needs member sizes and bool masks (generate it with --sdk)");

namespace LayoutCheck
{
    namespace
    {
        // Everything the walk needs and produces. Keys are table paths:
        // Module.Name for a class or struct, Module.Class.Function for a
        // function's parameters.
        struct Search
        {
            std::unordered_map<std::string, SDK::UStruct*> structs;                    // seeded with every wanted path
            std::unordered_map<std::string, std::vector<std::string>> functionOwners;  // class path -> function paths in it
            std::unordered_map<std::string, LiveStruct> live;
            int32_t objects = 0;
            int32_t types = 0;      // classes and structs looked up by name
            double  walkMs = 0.0;
        };

        // "/Script/Chimera" -> "Chimera"; anything that is not a native
        // package -> empty.
        const std::string& ModuleOf(const SDK::UObject* package,
            std::unordered_map<const SDK::UObject*, std::string>& cache)
        {
            auto it = cache.find(package);
            if (it == cache.end())
            {
                constexpr char kScript[] = "/Script/";
                std::string name = package->Name.GetRawString();
                it = cache.emplace(package, name.rfind(kScript, 0) == 0 ? name.substr(sizeof(kScript) - 1) : std::string()).first;
            }
            return it->second;
        }

        LiveStruct Snapshot(const SDK::UStruct* type)
        {
            LiveStruct live{ type->Size, type->MinAlignment, {} };
            for (const SDK::FField* field = type->ChildProperties; field; field = field->Next)
            {
                const auto* property = static_cast<const SDK::FProperty*>(field);
                const bool isBool = field->ClassPrivate
                    && (field->ClassPrivate->CastFlags & static_cast<uint64_t>(SDK::EClassCastFlags::BoolProperty)) != 0;
                live.properties.push_back(LiveProperty{
                    field->Name.GetRawString(),
                    property->Offset,
                    property->ElementSize * property->ArrayDim,
                    isBool,
                    isBool ? static_cast<const SDK::FBoolProperty*>(field)->FieldMask : 0u });
            }
            return live;
        }

        // One pass over GObjects: every native class and script struct that
        // sits directly in a /Script/ package is named Module.Name and looked
        // up; nothing else has its name decoded. Function parameter blocks are
        // then found in their owning class's Children.
        void Collect(Search& search)
        {
            std::unordered_map<const SDK::UObject*, std::string> modules;

            const auto walkStart = std::chrono::steady_clock::now();
            SDK::TUObjectArray* objects = SDK::UObject::GObjects.GetTypedPtr();
            search.objects = objects->Num();
            for (int32_t i = 0; i < search.objects; ++i)
            {
                SDK::UObject* object = objects->GetByIndex(i);
                if (!object || !object->Class
                    || !object->HasTypeFlag(SDK::EClassCastFlags::Class | SDK::EClassCastFlags::ScriptStruct))
                    continue;

                const SDK::UObject* package = object->Outer;
                if (!package || package->Outer)
                    continue;

                const std::string& module = ModuleOf(package, modules);
                if (module.empty())
                    continue;

                ++search.types;
                auto it = search.structs.find(module + "." + object->Name.GetRawString());
                if (it != search.structs.end() && !it->second)
                    it->second = static_cast<SDK::UStruct*>(object);
            }
            search.walkMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - walkStart).count();

            for (const auto& [classPath, functionPaths] : search.functionOwners)
            {
                const SDK::UStruct* owner = search.structs[classPath];
                if (!owner)
                    continue;
                for (SDK::UField* child = owner->Children; child; child = child->Next)
                {
                    if (!child->HasTypeFlag(SDK::EClassCastFlags::Function))
                        continue;
                    auto it = search.structs.find(classPath + "." + child->Name.GetRawString());
                    if (it != search.structs.end() && !it->second)
                        it->second = static_cast<SDK::UStruct*>(child);
                }
            }

            for (const auto& [path, type] : search.structs)
            {
                if (type)
                    search.live.emplace(path, Snapshot(type));
            }
        }

        // Collect under SEH: reflection data that is not what the SDK says it
        // is ends the check with a log line instead of taking the game down.
        bool CollectGuarded(Search* search, DWORD* exceptionCode)
        {
            __try
            {
                Collect(*search);
                return true;
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                *exceptionCode = GetExceptionCode();
                return false;
            }
        }
    }

    void Run()
    {
        const auto start = std::chrono::steady_clock::now();

        if (!SdkGlobals::VerifyGObjects())
        {
            ModLoaderLogger::LogError(L"[LayoutCheck] Skipped: GObjects does not look valid");
            return;
        }

        const std::vector<TableClass> table = ReadTable(SrLayoutTable::kNames, SrLayoutTable::kClasses, SrLayoutTable::kRows);

        Search search;
        for (const TableClass& sdk : table)
        {
            if (sdk.kind != Kind::Class && sdk.kind != Kind::ScriptStruct && sdk.kind != Kind::FunctionParams)
                continue;
            search.structs.emplace(sdk.path, nullptr);
            if (sdk.kind == Kind::FunctionParams)
            {
                const std::string path = sdk.path;
                const std::string classPath = path.substr(0, path.rfind('.'));
                search.structs.emplace(classPath, nullptr);
                search.functionOwners[classPath].push_back(path);
            }
        }

        DWORD exceptionCode = 0;
        if (!CollectGuarded(&search, &exceptionCode))
        {
            ModLoaderLogger::LogError(L"[LayoutCheck] Skipped: exception 0x%08lX while reading reflection data", exceptionCode);
            return;
        }

        const Result result = Compare(table, [&search](const TableClass& sdk) -> const LiveStruct*
        {
            const auto it = search.live.find(sdk.path);
            return it == search.live.end() ? nullptr : &it->second;
        });

        for (const std::string& line : result.lines)
            ModLoaderLogger::LogWarn(L"[LayoutCheck] %S", line.c_str());

        const double totalMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        void (*const log)(const wchar_t*, ...) =
            result.mismatched == 0 && result.missing == 0 ? ModLoaderLogger::LogInfo : ModLoaderLogger::LogWarn;
        log(
            L"[LayoutCheck] SDK CL %u against the game: %d classes, %d checked, %d mismatched, %d missing, %d deferred"
            L" -- GObjects pass %.1f ms (%d objects, %d types), %.1f ms in all",
            SrLayoutTable::kBuiltCL, result.classes, result.checked, result.mismatched, result.missing, result.deferred,
            search.walkMs, search.objects, search.types, totalMs);
    }
}

#endif

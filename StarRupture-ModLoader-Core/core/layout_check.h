#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

// Startup layout check.
//
// layout_table.generated.h lists the Game-SDK classes the loader uses, with
// the size of each and the offset, size and bool mask of each member, as the
// SDK the loader was built against has them (tools\Update-LayoutTable.ps1
// regenerates it). Run() walks the running game's reflection data once and
// logs every class whose layout no longer matches: after a game update that
// is the list of SDK types that now read the wrong bytes.
//
// It only logs. Nothing is disabled or blocked on its result.
namespace LayoutCheck
{
    // Called once from the init thread after FEngineLoop::Init has returned,
    // while the game thread is still parked in the engine-init hook and before
    // any plugin loads. Client builds only: layout_check.cpp is not compiled
    // into the server build, and without MODLOADER_CLIENT_BUILD (the offline
    // test) only the comparison below is.
    void Run();

    // ---- The comparison, kept apart from the game so it can be tested ------

    // Class::kind in the generated table.
    enum class Kind : uint32_t
    {
        Unknown        = 0,
        Class          = 1,
        ScriptStruct   = 2,
        FunctionParams = 3,
        Blueprint      = 4,
    };

    struct TableRow
    {
        const char* name;
        uint32_t    offset;
        uint32_t    size;       // 0: unknown, not compared
        uint32_t    boolMask;   // 0: not a bool
    };

    struct TableClass
    {
        const char*           name;     // the SDK's C++ name, ACrCharacterPlayerBase
        const char*           path;     // Module.Name, or Module.Class.Function for Kind::FunctionParams
        uint32_t              sizeOf;
        uint32_t              alignOf;  // 0: unknown, not compared
        Kind                  kind;
        std::vector<TableRow> rows;
    };

    // Reads a generated table: ReadTable(SrLayoutTable::kNames,
    // SrLayoutTable::kClasses, SrLayoutTable::kRows).
    template <class ClassT, size_t ClassCount, class RowT, size_t RowCount, size_t NameCount>
    std::vector<TableClass> ReadTable(const char* const (&names)[NameCount],
        const ClassT (&classes)[ClassCount], const RowT (&rows)[RowCount])
    {
        const auto name = [&](uint32_t index) { return index < NameCount ? names[index] : ""; };

        std::vector<TableClass> table;
        table.reserve(ClassCount);
        for (const ClassT& c : classes)
        {
            TableClass& out = table.emplace_back(TableClass{ name(c.name), name(c.path), c.sizeOf, c.alignOf,
                static_cast<Kind>(c.kind), {} });
            for (uint32_t i = c.firstRow; i < c.firstRow + c.rowCount && i < RowCount; ++i)
                out.rows.push_back(TableRow{ name(rows[i].name), rows[i].offset, rows[i].size, rows[i].boolMask });
        }
        return table;
    }

    // What the game reflects for one struct.
    struct LiveProperty
    {
        std::string name;
        int32_t     offset;
        int32_t     size;       // ElementSize * ArrayDim
        bool        isBool;
        uint32_t    fieldMask;  // FBoolProperty::FieldMask when isBool
    };

    struct LiveStruct
    {
        int32_t                   size;          // UStruct::PropertiesSize
        int32_t                   minAlignment;
        std::vector<LiveProperty> properties;    // its own ChildProperties, not its super's
    };

    struct Result
    {
        int classes    = 0;  // in the table
        int checked    = 0;  // found in the game and compared
        int mismatched = 0;  // found, but the layout differs
        int missing    = 0;  // not found in the game
        int deferred   = 0;  // not checked at startup (Blueprint classes, unknown kind)
        std::vector<std::string> changedClasses;  // SDK names of the mismatched and missing classes
        std::vector<std::string> lines;           // one per difference, ready to log
    };

    // Compares every table class with what `find` returns for it (nullptr:
    // not in the game).
    Result Compare(const std::vector<TableClass>& table,
        const std::function<const LiveStruct*(const TableClass&)>& find);
}

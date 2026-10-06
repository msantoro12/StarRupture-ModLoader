#include "pch.h"

#ifdef MODLOADER_CLIENT_BUILD
#include "native_settings_spike.h"
#include "object_ref.h"
#include "logging/logger.h"
#include "plugins/pak_registry.h"
#include "utils/game_thread_dispatch.h"
#include "../../hooks_common.h"
#include "../ufunction_resolve.h"
#include "../game_instance_init/game_instance_init.h"
#include "../text_localization/text_key.h"
#include "../text_localization/text_localization.h"

#include "Engine_classes.hpp"
#include "UMG_classes.hpp"
#include "CommonUI_classes.hpp"
#include "CommonUI_parameters.hpp"
#include "CommonGame_classes.hpp"
#include "CommonGame_parameters.hpp"
#include "GameSettings_classes.hpp"
#include "ChimeraUI_classes.hpp"
#include "ChimeraUI_parameters.hpp"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

namespace NativeSettingsSpike
{
	namespace
	{
		// -------------------------------------------------------------------
		// Constants
		// -------------------------------------------------------------------

		// FFrame as ModLoaderHello reads it: vptr, Node, Object, Code, Locals.
		// Code is null when the call came through ProcessEvent (a dynamic
		// delegate), and Locals then holds the parameters in declaration order.
		constexpr size_t kFFrameCodeOffset   = 0x18;
		constexpr size_t kFFrameLocalsOffset = 0x20;

		constexpr uint32_t kFuncNative = 0x400;

		constexpr uint32_t kSkipFlags =
			static_cast<uint32_t>(SDK::EObjectFlags::ClassDefaultObject) |
			static_cast<uint32_t>(SDK::EObjectFlags::ArchetypeObject) |
			static_cast<uint32_t>(SDK::EObjectFlags::BeginDestroyed) |
			static_cast<uint32_t>(SDK::EObjectFlags::FinishDestroyed) |
			static_cast<uint32_t>(SDK::EObjectFlags::MirroredGarbage);

		// UCrCustomGameSubsystem keeps 0x28 bytes of unreflected state after
		// UGameInstanceSubsystem. Logged before and after, never written.
		constexpr size_t kSubsystemPadOffset = 0x30;
		constexpr size_t kSubsystemPadSize   = 0x28;

		// The unreflected tail of each option row (Pad_448 / Pad_450).
		constexpr size_t kRowPadSize = 0x38;

		constexpr int    kMaxRows          = 16;
		constexpr int    kActivateTimeout  = 120;   // ticks to wait for the pushed page to activate
		constexpr int    kRecheckTicks     = 30;    // second child-count check after building
		constexpr double kStatsPeriodSec   = 5.0;   // tick-delta report while the page is open

		// -------------------------------------------------------------------
		// Cross-thread flags
		// -------------------------------------------------------------------
		std::atomic<bool> g_openRequested{false};
		std::atomic<bool> g_keyCaptureRequested{false};
		std::atomic<bool> g_disableRequested{false};
		std::mutex        g_forgottenMutex;
		std::string       g_forgottenPlugin;   // under g_forgottenMutex

		// -------------------------------------------------------------------
		// Page state. Game thread only.
		// -------------------------------------------------------------------
		enum class RowKind : uint8_t { CategoryHeading, TextHeading, Toggle, Slider, Rotator, KeyCapture };
		enum class Mode : uint8_t { None, Observe, Claim };

		struct Row
		{
			ObjectRef<SDK::UWidget> ref;
			const void*  raw         = nullptr;   // compared with a detour's `this`, never dereferenced
			RowKind      kind        = RowKind::Toggle;
			Mode         mode        = Mode::None;
			const char*  label       = "";
			const char*  description = "";
			size_t       padOffset   = 0;         // 0 for headings
			int          events      = 0;
		};

		struct HiddenStock
		{
			ObjectRef<SDK::UWidget> ref;
			SDK::ESlateVisibility   visibility;
		};

		enum class Phase { Idle, Pushed, Built };

		Phase                                                 g_phase = Phase::Idle;
		ObjectRef<SDK::UCommonActivatableWidget>              g_page;
		ObjectRef<SDK::UCommonActivatableWidgetContainerBase> g_container;
		ObjectRef<SDK::UCommonActivatableWidget>              g_keyPanel;
		std::vector<Row>                                      g_rows;
		std::vector<HiddenStock>                              g_hiddenStock;
		uint64_t     g_tick        = 0;
		uint64_t     g_phaseTick   = 0;
		int          g_cycle       = 0;
		int          g_childrenAfterBuild = -1;
		int          g_lastHover   = -1;
		bool         g_s1Done      = false;
		bool         g_broken      = false;   // an exception was caught; the spike stops acting
		bool         g_building    = false;   // handler events during the build are labelled as such
		uint8_t      g_subsystemBefore[kSubsystemPadSize]{};
		bool         g_haveSubsystemBefore = false;
		wchar_t      g_lastKeyText[128]{};

		// Tick-delta statistics while the page is open (S1).
		double g_statsElapsed = 0.0;
		int    g_statsFrames  = 0;
		float  g_statsMin     = 0.0f;
		float  g_statsMax     = 0.0f;

		// Hover events land here from the ProcessEvent observer (game thread)
		// and are applied to the description pane on the next tick.
		int g_pendingHover = -1;

		// -------------------------------------------------------------------
		// Handlers
		// -------------------------------------------------------------------
		enum HandlerId { kToggleOn, kToggleOff, kSlider, kRotator, kHandlerCount };

		struct Handler
		{
			const char*            className;
			const char*            funcName;
			uintptr_t              exec       = 0;
			uintptr_t              member     = 0;
			bool                   tailCall   = false;
			const wchar_t*         verdict    = L"not resolved";
			int                    directCallers = -1;
			std::vector<uintptr_t> calls;          // direct call targets inside the exec stub
			Hooks::Hook            thunkHook;
			Hooks::Hook            memberHook;
			void*                  thunkOrig  = nullptr;
			void*                  memberOrig = nullptr;
		};

		Handler g_handlers[kHandlerCount] = {
			{ "CrUW_WidgetOptionToggle", "OnButtonOnClicked" },
			{ "CrUW_WidgetOptionToggle", "OnButtonOffClicked" },
			{ "CrUW_WidgetOptionSlider", "OnSliderValueChanged" },
			{ "CrUW_WidgetOptionRotator", "OnRotatorValueChanged" },
		};

		bool        g_installed      = false;
		SDK::FName  g_hoverEventName{};   // SetHoverVisuals; a Blueprint override is a different UFunction with the same name
		bool        g_hoverObserving = false;

		// -------------------------------------------------------------------
		// Small helpers
		// -------------------------------------------------------------------
		bool IsLiveInstance(const SDK::UObject* obj)
		{
			return obj && obj->Class && (static_cast<uint32_t>(obj->Flags) & kSkipFlags) == 0;
		}

		template<typename Fn>
		void ForEachLiveObject(Fn fn)
		{
			const int32_t count = SDK::UObject::GObjects->Num();
			for (int32_t i = 0; i < count; ++i)
			{
				SDK::UObject* obj = SDK::UObject::GObjects->GetByIndex(i);
				if (!IsLiveInstance(obj))
					continue;
				if (!fn(obj))
					return;
			}
		}

		std::string NameOf(const SDK::UObject* obj)
		{
			return obj ? obj->GetName() : std::string("<null>");
		}

		std::string ClassNameOf(const SDK::UObject* obj)
		{
			return (obj && obj->Class) ? obj->Class->GetName() : std::string("<null>");
		}

		void FormatHex(const uint8_t* data, size_t size, wchar_t* out, size_t outCount)
		{
			size_t written = 0;
			out[0] = L'\0';
			for (size_t i = 0; i < size && written + 4 < outCount; ++i)
			{
				const int n = swprintf_s(out + written, outCount - written, i ? L" %02X" : L"%02X", data[i]);
				if (n <= 0)
					break;
				written += static_cast<size_t>(n);
			}
		}

		// -------------------------------------------------------------------
		// Structured-exception guards. Kept free of C++ objects so __try is
		// allowed here; the callee may have them. A spike that hits an access
		// violation should log it and stop, not take the game down.
		// -------------------------------------------------------------------
		using GuardedFn = void (*)(void* ctx);

		bool RunGuarded(GuardedFn fn, void* ctx, unsigned long* code)
		{
			__try
			{
				fn(ctx);
				return true;
			}
			__except (*code = GetExceptionCode(), EXCEPTION_EXECUTE_HANDLER)
			{
				return false;
			}
		}

		bool CallOriginal0(void* fn, void* self, unsigned long* code)
		{
			__try
			{
				reinterpret_cast<void(__fastcall*)(void*)>(fn)(self);
				return true;
			}
			__except (*code = GetExceptionCode(), EXCEPTION_EXECUTE_HANDLER)
			{
				return false;
			}
		}

		bool CallOriginalFloat(void* fn, void* self, float value, unsigned long* code)
		{
			__try
			{
				reinterpret_cast<void(__fastcall*)(void*, float)>(fn)(self, value);
				return true;
			}
			__except (*code = GetExceptionCode(), EXCEPTION_EXECUTE_HANDLER)
			{
				return false;
			}
		}

		bool CallOriginalIntBool(void* fn, void* self, int32_t value, bool flag, unsigned long* code)
		{
			__try
			{
				reinterpret_cast<void(__fastcall*)(void*, int32_t, bool)>(fn)(self, value, flag);
				return true;
			}
			__except (*code = GetExceptionCode(), EXCEPTION_EXECUTE_HANDLER)
			{
				return false;
			}
		}

		bool CallOriginalThunk(void* fn, void* context, void* stack, void* result, unsigned long* code)
		{
			__try
			{
				reinterpret_cast<void(__fastcall*)(void*, void*, void*)>(fn)(context, stack, result);
				return true;
			}
			__except (*code = GetExceptionCode(), EXCEPTION_EXECUTE_HANDLER)
			{
				return false;
			}
		}

		// Reads an FText's source string without taking a reference. The layout
		// is the SDK's predefined FTextData, which is a guess about engine
		// internals, so the read is guarded.
		struct FStringRaw
		{
			const wchar_t* Data;
			int32_t        Num;
			int32_t        Max;
		};

		bool ReadTextGuarded(const SDK::FText* text, wchar_t* out, size_t outCount)
		{
			out[0] = L'\0';
			__try
			{
				const auto* data = reinterpret_cast<const uint8_t*>(text->TextData);
				if (!data)
					return true;
				const auto* str = reinterpret_cast<const FStringRaw*>(
					data + offsetof(SDK::FTextImpl::FTextData, TextSource));
				if (!str->Data || str->Num <= 0)
					return true;
				size_t n = static_cast<size_t>(str->Num);
				if (n > outCount)
					n = outCount;
				size_t i = 0;
				for (; i + 1 < n && str->Data[i]; ++i)
					out[i] = str->Data[i];
				out[i] = L'\0';
				return true;
			}
			__except (EXCEPTION_EXECUTE_HANDLER)
			{
				out[0] = L'\0';
				return false;
			}
		}

		bool ScanCallersGuarded(const uint8_t* begin, size_t size, const uintptr_t* targets, int* counts, int count)
		{
			__try
			{
				for (size_t i = 0; i + 5 <= size; ++i)
				{
					if (begin[i] != 0xE8)
						continue;
					const int32_t rel = *reinterpret_cast<const int32_t*>(begin + i + 1);
					const uintptr_t target = reinterpret_cast<uintptr_t>(begin + i + 5) + static_cast<intptr_t>(rel);
					for (int t = 0; t < count; ++t)
					{
						if (targets[t] && target == targets[t])
							++counts[t];
					}
				}
				return true;
			}
			__except (EXCEPTION_EXECUTE_HANDLER)
			{
				return false;
			}
		}

		// -------------------------------------------------------------------
		// Text
		//
		// The same construction as GameMenu's EnsureText: one FText per label,
		// built through the loader's FText constructor hooks and kept for the
		// life of the process. Cached FTexts only ever reach a widget through a
		// reflected setter, which copies them with proper reference counting.
		// -------------------------------------------------------------------
		struct CachedText
		{
			char       label[128]{};
			SDK::FText text{};
		};
		std::vector<CachedText*> g_texts;   // never freed: each holds an FText reference for the process

		bool MakeText(const char* label, SDK::FText& out)
		{
			if (!label)
				return false;

			for (CachedText* cached : g_texts)
			{
				if (strcmp(cached->label, label) == 0)
				{
					out = cached->text;
					return true;
				}
			}

			const uintptr_t keyCtor  = Hooks::TextKey::GetOriginalPtr();
			const uintptr_t textCtor = Hooks::TextLocalization::GetOriginalPtr();
			if (!keyCtor || !textCtor)
			{
				ModLoaderLogger::LogWarn(L"[NativeSettingsSpike] FText helpers are unavailable -- labels stay blank");
				return false;
			}

			auto MakeKey  = reinterpret_cast<void(__fastcall*)(void*, const wchar_t*)>(keyCtor);
			auto MakeFText = reinterpret_cast<SDK::FText*(__fastcall*)(
				SDK::FText*, const void*, const void*, const wchar_t*)>(textCtor);

			wchar_t wide[128]{};
			for (int i = 0; i < 127 && label[i]; ++i)
				wide[i] = static_cast<wchar_t>(static_cast<unsigned char>(label[i]));

			uint8_t namespaceKey[32]{};
			uint8_t textKey[32]{};
			MakeKey(namespaceKey, L"ModLoader");
			MakeKey(textKey, wide);

			auto* cached = new CachedText();
			MakeFText(&cached->text, namespaceKey, textKey, wide);
			strncpy_s(cached->label, sizeof(cached->label), label, _TRUNCATE);
			g_texts.push_back(cached);

			out = cached->text;
			return true;
		}

		void SetTextBlock(SDK::UTextBlock* block, const char* label)
		{
			SDK::FText text{};
			if (block && MakeText(label, text))
				block->SetText(text);
		}

		std::wstring ReadTextBlock(const SDK::UTextBlock* block)
		{
			wchar_t buffer[128]{};
			if (block)
				ReadTextGuarded(&block->Text, buffer, _countof(buffer));
			return buffer;
		}

		// -------------------------------------------------------------------
		// Reflected calls into modules whose SDK bodies the loader does not
		// compile (CommonUI, CommonGame, ChimeraUI). Same shape as the SDK's
		// generated bodies: find the UFunction on the object's class chain,
		// set FUNC_Native for the duration if it is a native function, and call
		// ProcessEvent with the SDK's own Params struct.
		// -------------------------------------------------------------------
		bool CallUFunction(SDK::UObject* obj, const char* className, const char* funcName, void* parms, bool native)
		{
			if (!obj || !obj->Class)
				return false;

			SDK::UFunction* fn = obj->Class->GetFunction(className, funcName);
			if (!fn)
			{
				ModLoaderLogger::LogWarn(L"[NativeSettingsSpike] %S::%S not found on %S",
				                         className, funcName, ClassNameOf(obj).c_str());
				return false;
			}

			using FlagsT = decltype(fn->FunctionFlags);
			const auto flags = fn->FunctionFlags;
			if (native)
				fn->FunctionFlags = static_cast<FlagsT>(static_cast<uint32_t>(flags) | kFuncNative);
			obj->ProcessEvent(fn, parms);
			fn->FunctionFlags = flags;
			return true;
		}

		bool IsActivated(const SDK::UCommonActivatableWidget* widget)
		{
			return widget && widget->bIsActive;
		}

		// -------------------------------------------------------------------
		// Row bookkeeping
		// -------------------------------------------------------------------
		Row* FindRow(const void* self)
		{
			if (!self)
				return nullptr;
			for (Row& row : g_rows)
			{
				if (row.raw != self)
					continue;
				// Same address. Only ours if the slot still holds it.
				return static_cast<const void*>(row.ref.Get()) == self ? &row : nullptr;
			}
			return nullptr;
		}

		int RowIndex(const Row* row)
		{
			return row ? static_cast<int>(row - g_rows.data()) : -1;
		}

		const wchar_t* ModeText(Mode mode)
		{
			switch (mode)
			{
			case Mode::Observe: return L"observe";
			case Mode::Claim:   return L"claim";
			default:            return L"none";
			}
		}

		void LogRowPad(const Row& row, const wchar_t* when)
		{
			if (!row.padOffset)
				return;
			SDK::UWidget* widget = row.ref.Get();
			if (!widget)
				return;
			wchar_t hex[kRowPadSize * 3 + 8];
			FormatHex(reinterpret_cast<const uint8_t*>(widget) + row.padOffset, kRowPadSize, hex, _countof(hex));
			ModLoaderLogger::LogInfo(L"[NativeSettingsSpike] S2: row %d '%S' unreflected state %s: %s",
			                         RowIndex(&row), row.label, when, hex);
		}

		// What a claimed event does. Nothing reaches a store; the key row asks
		// the tick to open the key capture panel.
		void OnClaimed(Row& row, const wchar_t* what)
		{
			ModLoaderLogger::LogInfo(L"[NativeSettingsSpike] S2: row %d '%S' %s -- claimed, the original did not run",
			                         RowIndex(&row), row.label, what);
			if (row.kind == RowKind::KeyCapture)
			{
				g_keyCaptureRequested.store(true);
				ModLoaderLogger::LogInfo(L"[NativeSettingsSpike] S4: key capture requested from row %d", RowIndex(&row));
			}
		}

		// -------------------------------------------------------------------
		// Exec-thunk detours: see every call that arrives through ProcessEvent
		// (a dynamic delegate). Fixed signature, so they can never be attached
		// to the wrong function. They claim only when the member detour could
		// not be installed, and only for a ProcessEvent call (no bytecode to
		// step past).
		// -------------------------------------------------------------------
		using ThunkFn = void(__fastcall*)(void*, void*, void*);

		template<int Id>
		void __fastcall ThunkDetour(void* context, void* stack, void* result)
		{
			Handler& h = g_handlers[Id];
			Row* row = FindRow(context);
			if (!row)
			{
				reinterpret_cast<ThunkFn>(h.thunkOrig)(context, stack, result);
				return;
			}

			const auto* frame  = static_cast<const uint8_t*>(stack);
			const auto* code   = *reinterpret_cast<const uint8_t* const*>(frame + kFFrameCodeOffset);
			const auto* locals = *reinterpret_cast<const uint8_t* const*>(frame + kFFrameLocalsOffset);

			wchar_t value[64] = L"-";
			if (code)
				wcscpy_s(value, L"(bytecode call, not read)");
			else if (locals && Id == kSlider)
				swprintf_s(value, L"%.3f", *reinterpret_cast<const float*>(locals));
			else if (locals && Id == kRotator)
				swprintf_s(value, L"%d user=%d", *reinterpret_cast<const int32_t*>(locals),
				           static_cast<int>(locals[4]));

			++row->events;
			const bool claim = row->mode == Mode::Claim && !h.memberHook.installed && !code;
			ModLoaderLogger::LogInfo(
				L"[NativeSettingsSpike] S2: %S::%S on row %d '%S' via the exec thunk (ProcessEvent path)%s, value %s, mode %s",
				h.className, h.funcName, RowIndex(row), row->label, g_building ? L" during build" : L"",
				value, ModeText(row->mode));

			if (claim)
			{
				OnClaimed(*row, L"at the exec thunk");
				return;
			}

			unsigned long ex = 0;
			if (!CallOriginalThunk(h.thunkOrig, context, stack, result, &ex))
			{
				ModLoaderLogger::LogError(
					L"[NativeSettingsSpike] S2: exception 0x%08lX inside the original %S::%S for row %d -- "
					L"switching the row to claim mode", ex, h.className, h.funcName, RowIndex(row));
				row->mode = Mode::Claim;
			}
		}

		// -------------------------------------------------------------------
		// Member detours: the design's claim point (GameMenu's technique).
		// Installed only when the stub resolution passed every check below.
		// -------------------------------------------------------------------
		void ToggleMember(int id, void* self)
		{
			Handler& h = g_handlers[id];
			Row* row = FindRow(self);
			if (!row)
			{
				reinterpret_cast<void(__fastcall*)(void*)>(h.memberOrig)(self);
				return;
			}

			++row->events;
			ModLoaderLogger::LogInfo(L"[NativeSettingsSpike] S2: %S on row %d '%S' via the member detour%s, mode %s",
			                         h.funcName, RowIndex(row), row->label,
			                         g_building ? L" during build" : L"", ModeText(row->mode));
			if (row->mode == Mode::Claim)
			{
				OnClaimed(*row, id == kToggleOn ? L"ON clicked" : L"OFF clicked");
				return;
			}

			unsigned long ex = 0;
			if (!CallOriginal0(h.memberOrig, self, &ex))
			{
				ModLoaderLogger::LogError(
					L"[NativeSettingsSpike] S2: exception 0x%08lX inside the original %S for row %d -- "
					L"switching the row to claim mode", ex, h.funcName, RowIndex(row));
				row->mode = Mode::Claim;
				return;
			}
			ModLoaderLogger::LogInfo(L"[NativeSettingsSpike] S2: original %S returned for row %d", h.funcName, RowIndex(row));
			LogRowPad(*row, L"after the original");
		}

		void __fastcall ToggleOnMemberDetour(void* self)  { ToggleMember(kToggleOn, self); }
		void __fastcall ToggleOffMemberDetour(void* self) { ToggleMember(kToggleOff, self); }

		void __fastcall SliderMemberDetour(void* self, float value)
		{
			Handler& h = g_handlers[kSlider];
			Row* row = FindRow(self);
			if (!row)
			{
				reinterpret_cast<void(__fastcall*)(void*, float)>(h.memberOrig)(self, value);
				return;
			}

			++row->events;
			ModLoaderLogger::LogInfo(L"[NativeSettingsSpike] S2: OnSliderValueChanged(%.3f) on row %d '%S' via the member detour%s, mode %s",
			                         value, RowIndex(row), row->label, g_building ? L" during build" : L"", ModeText(row->mode));
			if (row->mode == Mode::Claim)
			{
				OnClaimed(*row, L"slider moved");
				return;
			}

			unsigned long ex = 0;
			if (!CallOriginalFloat(h.memberOrig, self, value, &ex))
			{
				ModLoaderLogger::LogError(
					L"[NativeSettingsSpike] S2: exception 0x%08lX inside the original OnSliderValueChanged for row %d -- "
					L"switching the row to claim mode", ex, RowIndex(row));
				row->mode = Mode::Claim;
				return;
			}
			LogRowPad(*row, L"after the original");
		}

		void __fastcall RotatorMemberDetour(void* self, int32_t value, bool userInitiated)
		{
			Handler& h = g_handlers[kRotator];
			Row* row = FindRow(self);
			if (!row)
			{
				reinterpret_cast<void(__fastcall*)(void*, int32_t, bool)>(h.memberOrig)(self, value, userInitiated);
				return;
			}

			++row->events;
			ModLoaderLogger::LogInfo(L"[NativeSettingsSpike] S2: OnRotatorValueChanged(%d, user=%d) on row %d '%S' via the member detour%s, mode %s",
			                         value, userInitiated ? 1 : 0, RowIndex(row), row->label,
			                         g_building ? L" during build" : L"", ModeText(row->mode));
			if (row->mode == Mode::Claim)
			{
				OnClaimed(*row, L"rotated");
				return;
			}

			unsigned long ex = 0;
			if (!CallOriginalIntBool(h.memberOrig, self, value, userInitiated, &ex))
			{
				ModLoaderLogger::LogError(
					L"[NativeSettingsSpike] S2: exception 0x%08lX inside the original OnRotatorValueChanged for row %d -- "
					L"switching the row to claim mode", ex, RowIndex(row));
				row->mode = Mode::Claim;
				return;
			}
			LogRowPad(*row, L"after the original");
		}

		void* const kThunkDetours[kHandlerCount] = {
			reinterpret_cast<void*>(&ThunkDetour<kToggleOn>),
			reinterpret_cast<void*>(&ThunkDetour<kToggleOff>),
			reinterpret_cast<void*>(&ThunkDetour<kSlider>),
			reinterpret_cast<void*>(&ThunkDetour<kRotator>),
		};

		void* const kMemberDetours[kHandlerCount] = {
			reinterpret_cast<void*>(&ToggleOnMemberDetour),
			reinterpret_cast<void*>(&ToggleOffMemberDetour),
			reinterpret_cast<void*>(&SliderMemberDetour),
			reinterpret_cast<void*>(&RotatorMemberDetour),
		};

		// -------------------------------------------------------------------
		// Hover (SetHoverVisuals is a BlueprintEvent, so it passes through
		// ProcessEvent). Called for every ProcessEvent in the game: a name
		// compare first, nothing else unless it matches.
		// -------------------------------------------------------------------
		void HoverObserver(void* obj, void* fn, void* params)
		{
			if (!fn || !obj)
				return;
			const auto* function = static_cast<const SDK::UObject*>(fn);
			if (function->Name.ComparisonIndex != g_hoverEventName.ComparisonIndex
			    || function->Name.Number != g_hoverEventName.Number)
				return;
			if (!GameThreadDispatch::IsGameThread())
				return;

			Row* row = FindRow(obj);
			if (!row)
				return;

			const bool hovered = params && *static_cast<const bool*>(params);
			ModLoaderLogger::LogInfo(L"[NativeSettingsSpike] S2: hover %s on row %d '%S'",
			                         hovered ? L"on" : L"off", RowIndex(row), row->label);
			if (hovered)
				g_pendingHover = RowIndex(row);
		}

		// -------------------------------------------------------------------
		// Exec stub -> member resolution, stricter than GameMenu's walk:
		//   - stops at ret, at an int3, or at a jump that leaves the stub;
		//   - a jmp rel32 out of the stub is a tail call and its target is
		//     the member;
		//   - otherwise the member is the last call rel32 before ret, and only
		//     epilogue instructions may follow it.
		// The caller then refuses a target that another stub also calls (a
		// shared helper such as FFrame::Step) or that has many direct callers.
		// -------------------------------------------------------------------
		bool IsEpilogueInstruction(const uint8_t* insn, size_t length)
		{
			const uint8_t op = insn[0];
			if (op == 0x90)                                          return true;   // nop
			if (op >= 0x58 && op <= 0x5F)                            return true;   // pop r64
			if (op == 0x41 && insn[1] >= 0x58 && insn[1] <= 0x5F)    return true;   // pop r8-r15
			if (op == 0x48 && insn[1] == 0x83 && insn[2] == 0xC4)    return true;   // add rsp, imm8
			if (op == 0x48 && insn[1] == 0x81 && insn[2] == 0xC4)    return true;   // add rsp, imm32
			if ((op == 0x48 || op == 0x4C) && insn[1] == 0x8B && length >= 4
			    && (insn[2] & 7) == 4 && (insn[3] & 7) == 4)         return true;   // mov r64, [rsp+disp]
			if (op == 0x0F && insn[1] == 0x28)                       return true;   // movaps xmm, [rsp+disp]
			if (op == 0x44 && insn[1] == 0x0F && insn[2] == 0x28)    return true;   // movaps xmm8+, [rsp+disp]
			if (op == 0x0F && insn[1] == 0x1F)                       return true;   // multi-byte nop
			if (op == 0x66 && (insn[1] == 0x90 || insn[1] == 0x0F))  return true;   // 66 nop forms
			return false;
		}

		void DecodeStub(Handler& h)
		{
			const auto* code = reinterpret_cast<const uint8_t*>(h.exec);
			uintptr_t lastCall    = 0;
			size_t    lastCallEnd = 0;
			size_t    offset      = 0;
			bool      reachedRet  = false;

			while (offset < 512)
			{
				const uint8_t* insn = code + offset;
				const uint8_t  op   = insn[0];

				if (op == 0xC3 || op == 0xC2 || (op == 0xF3 && insn[1] == 0xC3))
				{
					reachedRet = true;
					break;
				}
				if (op == 0xCC)
				{
					h.verdict = L"refused: reached int3 padding before a ret";
					return;
				}

				const size_t length = Hooks::GetInstructionLength(insn);
				if (length == 0 || length > 15)
				{
					h.verdict = L"refused: could not decode the stub";
					return;
				}

				if (op == 0xE8 && length == 5)
				{
					const int32_t rel = *reinterpret_cast<const int32_t*>(insn + 1);
					lastCall    = reinterpret_cast<uintptr_t>(insn + 5) + static_cast<intptr_t>(rel);
					lastCallEnd = offset + length;
					h.calls.push_back(lastCall);
				}
				else if (op == 0xE9 && length == 5)
				{
					const int32_t rel = *reinterpret_cast<const int32_t*>(insn + 1);
					const uintptr_t target = reinterpret_cast<uintptr_t>(insn + 5) + static_cast<intptr_t>(rel);
					if (target < h.exec || target >= h.exec + 0x400)
					{
						h.member   = target;
						h.tailCall = true;
						h.verdict  = L"tail call out of the stub";
						return;
					}
				}
				else if (op == 0xFF && length >= 2 && ((insn[1] >> 3) & 7) == 4)
				{
					h.verdict = L"refused: indirect jump in the stub";
					return;
				}

				offset += length;
			}

			if (!reachedRet)
			{
				h.verdict = L"refused: no ret within 512 bytes";
				return;
			}
			if (!lastCall)
			{
				h.verdict = L"refused: no call before the ret";
				return;
			}

			for (size_t o = lastCallEnd; o < offset; )
			{
				const uint8_t* insn = code + o;
				const size_t length = Hooks::GetInstructionLength(insn);
				if (length == 0 || length > 15 || !IsEpilogueInstruction(insn, length))
				{
					h.verdict = L"refused: the last call is followed by more than an epilogue";
					return;
				}
				o += length;
			}

			h.member  = lastCall;
			h.verdict = L"last call before ret";
		}

		bool InMainModule(uintptr_t addr, uintptr_t* textBegin = nullptr, size_t* textSize = nullptr)
		{
			HMODULE module = GetModuleHandleW(nullptr);
			if (!module || !addr)
				return false;
			const auto base = reinterpret_cast<uintptr_t>(module);
			auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(module);
			auto* nt  = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
			if (textBegin && textSize)
			{
				*textBegin = 0;
				*textSize  = 0;
				IMAGE_SECTION_HEADER* section = IMAGE_FIRST_SECTION(nt);
				for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++section)
				{
					if (strncmp(reinterpret_cast<const char*>(section->Name), ".text", 5) == 0)
					{
						*textBegin = base + section->VirtualAddress;
						*textSize  = section->Misc.VirtualSize;
						break;
					}
				}
			}
			return addr >= base && addr < base + nt->OptionalHeader.SizeOfImage;
		}

		void ResolveHandlers()
		{
			for (Handler& h : g_handlers)
			{
				h.exec = Hooks::ResolveUFunctionNativeAddr(h.className, h.funcName);
				if (h.exec)
					DecodeStub(h);
				else
					h.verdict = L"refused: UFunction not found";
			}

			// A target that another stub also calls is a shared helper, not a member.
			for (Handler& h : g_handlers)
			{
				if (!h.member)
					continue;
				if (!InMainModule(h.member))
				{
					h.member  = 0;
					h.verdict = L"refused: target outside the game module";
					continue;
				}
				for (const Handler& other : g_handlers)
				{
					if (&other == &h)
						continue;
					bool shared = other.member == h.member;
					for (uintptr_t call : other.calls)
						shared = shared || call == h.member;
					if (shared)
					{
						h.member  = 0;
						h.verdict = L"refused: another stub calls the same target (shared helper)";
						break;
					}
				}
			}

			// Direct callers across .text. A handler member has a handful; a
			// helper has hundreds.
			uintptr_t textBegin = 0;
			size_t    textSize  = 0;
			InMainModule(reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr)), &textBegin, &textSize);
			uintptr_t targets[kHandlerCount]{};
			int       counts[kHandlerCount]{};
			for (int i = 0; i < kHandlerCount; ++i)
				targets[i] = g_handlers[i].member;

			if (textBegin && textSize
			    && ScanCallersGuarded(reinterpret_cast<const uint8_t*>(textBegin), textSize, targets, counts, kHandlerCount))
			{
				for (int i = 0; i < kHandlerCount; ++i)
				{
					Handler& h = g_handlers[i];
					if (!h.member)
						continue;
					h.directCallers = counts[i];
					if (counts[i] > 8)
					{
						h.member  = 0;
						h.verdict = L"refused: the target has more than 8 direct callers (a helper)";
					}
				}
			}
			else
			{
				for (Handler& h : g_handlers)
				{
					h.member  = 0;
					h.verdict = L"refused: could not scan .text for callers";
				}
			}

			const auto base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
			for (const Handler& h : g_handlers)
			{
				ModLoaderLogger::LogInfo(
					L"[NativeSettingsSpike] S2: %S::%S exec=base+0x%llX member=%s0x%llX (%s) direct callers=%d",
					h.className, h.funcName,
					static_cast<unsigned long long>(h.exec ? h.exec - base : 0),
					h.member ? L"base+" : L"",
					static_cast<unsigned long long>(h.member ? h.member - base : 0),
					h.verdict, h.directCallers);
			}
		}

		// -------------------------------------------------------------------
		// S1: read-only dump
		// -------------------------------------------------------------------
		SDK::UObject* FindSubsystem()
		{
			SDK::UObject* found = nullptr;
			const SDK::UClass* cls = SDK::UObject::FindClassFast("CrCustomGameSubsystem");
			if (!cls)
				return nullptr;
			ForEachLiveObject([&](SDK::UObject* obj)
			{
				if (obj->Class == cls)
				{
					found = obj;
					return false;
				}
				return true;
			});
			return found;
		}

		bool ReadSubsystem(uint8_t (&out)[kSubsystemPadSize])
		{
			SDK::UObject* subsystem = FindSubsystem();
			if (!subsystem)
				return false;
			memcpy(out, reinterpret_cast<const uint8_t*>(subsystem) + kSubsystemPadOffset, kSubsystemPadSize);
			return true;
		}

		void LogSubsystem(const wchar_t* when)
		{
			uint8_t now[kSubsystemPadSize]{};
			if (!ReadSubsystem(now))
			{
				ModLoaderLogger::LogInfo(L"[NativeSettingsSpike] S2: UCrCustomGameSubsystem not found (%s)", when);
				return;
			}
			wchar_t hex[kSubsystemPadSize * 3 + 8];
			FormatHex(now, kSubsystemPadSize, hex, _countof(hex));
			ModLoaderLogger::LogInfo(L"[NativeSettingsSpike] S2: UCrCustomGameSubsystem state %s: %s", when, hex);

			if (wcscmp(when, L"before") == 0)
			{
				memcpy(g_subsystemBefore, now, kSubsystemPadSize);
				g_haveSubsystemBefore = true;
			}
			else if (g_haveSubsystemBefore)
			{
				ModLoaderLogger::LogInfo(L"[NativeSettingsSpike] S2: UCrCustomGameSubsystem state %s",
				                         memcmp(now, g_subsystemBefore, kSubsystemPadSize) == 0
				                             ? L"UNCHANGED" : L"CHANGED (see the two lines above)");
			}
		}

		std::string SoftPathString(const SDK::FSoftObjectPath& path)
		{
			const std::string package = path.AssetPath.PackageName.ToString();
			const std::string asset   = path.AssetPath.AssetName.ToString();
			if (package.empty() || package == "None")
				return std::string();
			return package + "." + asset;
		}

		void DumpWidgetsData(std::string* customGamePath)
		{
			const SDK::UClass* cls = SDK::UCrMenuWidgetsData::StaticClass();
			int assets = 0;
			ForEachLiveObject([&](SDK::UObject* obj)
			{
				if (!cls || !obj->IsA(cls))
					return true;
				++assets;
				auto* data = static_cast<SDK::UCrMenuWidgetsData*>(obj);
				ModLoaderLogger::LogInfo(L"[NativeSettingsSpike] S1: menu widgets data '%S' has %d entries",
				                         obj->GetFullName().c_str(), data->Widgets.Num());
				for (int i = 0; i < data->Widgets.Num(); ++i)
				{
					const SDK::FCrMenuConfig& config = data->Widgets[i];
					const std::string path = SoftPathString(config.WidgetClass);
					wchar_t title[96]{};
					ReadTextGuarded(&config.Title, title, _countof(title));
					ModLoaderLogger::LogInfo(L"[NativeSettingsSpike] S1:   MenuType %u -> %S (title '%s', overrides=%d)",
					                         static_cast<unsigned>(config.MenuType), path.c_str(), title,
					                         config.bOverridesBehaviour ? 1 : 0);
					if (config.MenuType == SDK::ECrMenuType::CustomGame && customGamePath && customGamePath->empty())
						*customGamePath = path;
				}
				return true;
			});
			if (assets == 0)
				ModLoaderLogger::LogInfo(L"[NativeSettingsSpike] S1: no UCrMenuWidgetsData is loaded");
		}

		// Loaded Blueprint subclasses of a native class (the dump has none:
		// Dumper-7 marked the natives final).
		std::vector<SDK::UClass*> FindLoadedSubclasses(const SDK::UClass* native)
		{
			std::vector<SDK::UClass*> found;
			if (!native)
				return found;
			const int32_t count = SDK::UObject::GObjects->Num();
			for (int32_t i = 0; i < count; ++i)
			{
				SDK::UObject* obj = SDK::UObject::GObjects->GetByIndex(i);
				if (!obj || !obj->Class || !obj->IsA(SDK::EClassCastFlags::Class))
					continue;
				auto* cls = static_cast<SDK::UClass*>(obj);
				if (cls != native && cls->IsSubclassOf(native))
					found.push_back(cls);
			}
			return found;
		}

		SDK::UClass* FindCustomGameClass()
		{
			std::string path;
			DumpWidgetsData(&path);

			const SDK::UClass* native = SDK::UCrUW_CustomGame::StaticClass();
			if (!path.empty())
			{
				IPluginPak* pak = PakRegistry::GetInterface();
				auto* cls = pak ? static_cast<SDK::UClass*>(pak->LoadClass(path.c_str())) : nullptr;
				if (cls && cls->IsSubclassOf(native))
				{
					ModLoaderLogger::LogInfo(L"[NativeSettingsSpike] S1: Custom Game page class found through WidgetsData: %S",
					                         cls->GetFullName().c_str());
					return cls;
				}
				ModLoaderLogger::LogWarn(L"[NativeSettingsSpike] S1: WidgetsData names %S but it did not load as a "
				                         L"UCrUW_CustomGame subclass", path.c_str());
			}

			const std::vector<SDK::UClass*> loaded = FindLoadedSubclasses(native);
			for (SDK::UClass* cls : loaded)
				ModLoaderLogger::LogInfo(L"[NativeSettingsSpike] S1: loaded UCrUW_CustomGame subclass: %S",
				                         cls->GetFullName().c_str());
			if (!loaded.empty())
			{
				ModLoaderLogger::LogInfo(L"[NativeSettingsSpike] S1: Custom Game page class found by scanning loaded classes");
				return loaded.front();
			}

			ModLoaderLogger::LogWarn(L"[NativeSettingsSpike] S1: Custom Game page class NOT found. Open New Session -> "
			                         L"Custom Game once, back out, and click MODS again.");
			return nullptr;
		}

		SDK::UClass* RowClassFor(SDK::UCrUW_CustomGame* cdo, SDK::ECrOptionType type)
		{
			auto& map = cdo->OptionsTypeWidgets;
			for (int i = 0; i < map.NumAllocated(); ++i)
			{
				if (!map.IsValidIndex(i))
					continue;
				auto& pair = map[i];
				if (pair.Key() == type)
					return pair.Value().Get();
			}
			return nullptr;
		}

		void DumpOptionsTypeWidgets(SDK::UClass* pageClass)
		{
			auto* cdo = static_cast<SDK::UCrUW_CustomGame*>(pageClass->ClassDefaultObject);
			if (!cdo)
			{
				ModLoaderLogger::LogWarn(L"[NativeSettingsSpike] S1: the page class has no default object");
				return;
			}
			auto& map = cdo->OptionsTypeWidgets;
			ModLoaderLogger::LogInfo(L"[NativeSettingsSpike] S1: OptionsTypeWidgets on the class default object: %d entr%s",
			                         map.Num(), map.Num() == 1 ? L"y" : L"ies");
			for (int i = 0; i < map.NumAllocated(); ++i)
			{
				if (!map.IsValidIndex(i))
					continue;
				auto& pair = map[i];
				SDK::UClass* cls = pair.Value().Get();
				ModLoaderLogger::LogInfo(L"[NativeSettingsSpike] S1:   ECrOptionType %u -> %S",
				                         static_cast<unsigned>(pair.Key()), cls ? cls->GetFullName().c_str() : "<null>");
			}
		}

		SDK::UCommonActivatableWidget* FindActiveMenu()
		{
			SDK::UCommonActivatableWidget* found = nullptr;
			const SDK::UClass* mainMenu  = SDK::UCrUW_MainMenuWidget::StaticClass();
			const SDK::UClass* pauseMenu = SDK::UCrUW_PauseMenu::StaticClass();
			ForEachLiveObject([&](SDK::UObject* obj)
			{
				if ((mainMenu && obj->IsA(mainMenu)) || (pauseMenu && obj->IsA(pauseMenu)))
				{
					auto* widget = static_cast<SDK::UCommonActivatableWidget*>(obj);
					ModLoaderLogger::LogInfo(L"[NativeSettingsSpike] S1: menu widget %S (%S) active=%d",
					                         NameOf(obj).c_str(), ClassNameOf(obj).c_str(), widget->bIsActive ? 1 : 0);
					if (!found && widget->bIsActive)
						found = widget;
				}
				return true;
			});
			return found;
		}

		// The container whose list holds the active menu, or the nearest
		// activatable widget above it (Outer is the parent's WidgetTree, whose
		// Outer is the parent widget).
		SDK::UCommonActivatableWidgetContainerBase* FindContainerFor(SDK::UCommonActivatableWidget* menu)
		{
			std::vector<SDK::UObject*> chain;
			for (SDK::UObject* obj = menu; obj && chain.size() < 12; obj = obj->Outer)
			{
				if (obj->IsA(SDK::UCommonActivatableWidget::StaticClass()))
					chain.push_back(obj);
			}

			SDK::UCommonActivatableWidgetContainerBase* best = nullptr;
			size_t bestDepth = SIZE_MAX;
			const SDK::UClass* containerClass = SDK::UCommonActivatableWidgetContainerBase::StaticClass();
			ForEachLiveObject([&](SDK::UObject* obj)
			{
				if (!containerClass || !obj->IsA(containerClass))
					return true;
				auto* container = static_cast<SDK::UCommonActivatableWidgetContainerBase*>(obj);
				std::string list;
				for (int i = 0; i < container->WidgetList.Num(); ++i)
				{
					SDK::UCommonActivatableWidget* widget = container->WidgetList[i];
					list += (i ? ", " : "") + NameOf(widget);
					for (size_t depth = 0; depth < chain.size(); ++depth)
					{
						if (chain[depth] == widget && depth < bestDepth)
						{
							best = container;
							bestDepth = depth;
						}
					}
				}
				ModLoaderLogger::LogInfo(L"[NativeSettingsSpike] S1: container %S (%S) outer %S, %d widget(s) [%S], displayed %S",
				                         NameOf(obj).c_str(), ClassNameOf(obj).c_str(), NameOf(obj->Outer).c_str(),
				                         container->WidgetList.Num(), list.c_str(), NameOf(container->DisplayedWidget).c_str());
				return true;
			});

			if (best)
				ModLoaderLogger::LogInfo(L"[NativeSettingsSpike] S1: the menu sits in container %S (%zu level(s) up)",
				                         NameOf(best).c_str(), bestDepth);
			return best;
		}

		// Layer tag containing `needle` (case-insensitive), from the first
		// PrimaryGameLayout. Logs every layer the first time.
		bool FindLayer(const char* needle, SDK::FGameplayTag* out, bool log)
		{
			bool found = false;
			const SDK::UClass* layoutClass = SDK::UPrimaryGameLayout::StaticClass();
			ForEachLiveObject([&](SDK::UObject* obj)
			{
				if (!layoutClass || !obj->IsA(layoutClass))
					return true;
				auto& layers = static_cast<SDK::UPrimaryGameLayout*>(obj)->Layers;
				if (log)
					ModLoaderLogger::LogInfo(L"[NativeSettingsSpike] S1: PrimaryGameLayout %S has %d layer(s)",
					                         NameOf(obj).c_str(), layers.Num());
				for (int i = 0; i < layers.NumAllocated(); ++i)
				{
					if (!layers.IsValidIndex(i))
						continue;
					auto& pair = layers[i];
					std::string tag = pair.Key().TagName.ToString();
					if (log)
						ModLoaderLogger::LogInfo(L"[NativeSettingsSpike] S1:   layer %S -> %S", tag.c_str(),
						                         NameOf(pair.Value()).c_str());
					std::string lower = tag;
					for (char& c : lower)
						c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
					if (!found && lower.find(needle) != std::string::npos)
					{
						*out  = pair.Key();
						found = true;
					}
				}
				return false;   // the first layout is the one
			});
			return found;
		}

		bool IsMainMenuWorld()
		{
			SDK::UWorld* world = SDK::UWorld::GetWorld();
			return world && world->GetName().find("MainMenu") != std::string::npos;
		}

		void LogPauseState(const wchar_t* when)
		{
			SDK::UWorld* world = SDK::UWorld::GetWorld();
			const bool paused = world && SDK::UGameplayStatics::IsGamePaused(world);
			ModLoaderLogger::LogInfo(L"[NativeSettingsSpike] S1: %s: world %S, IsGamePaused=%d", when,
			                         world ? world->GetName().c_str() : "<none>", paused ? 1 : 0);
		}

		// -------------------------------------------------------------------
		// Push
		// -------------------------------------------------------------------
		SDK::UCommonActivatableWidget* PushToContainer(SDK::UCommonActivatableWidgetContainerBase* container, SDK::UClass* cls)
		{
			SDK::Params::CommonActivatableWidgetContainerBase_BP_AddWidget parms{};
			parms.ActivatableWidgetClass = cls;
			if (!CallUFunction(container, "CommonActivatableWidgetContainerBase", "BP_AddWidget", &parms, true))
				return nullptr;
			return parms.ReturnValue;
		}

		SDK::UCommonActivatableWidget* PushToLayer(SDK::UCommonActivatableWidget* owner, const SDK::FGameplayTag& layer, SDK::UClass* cls)
		{
			SDK::ULocalPlayer* player = owner ? owner->GetOwningLocalPlayer() : nullptr;
			SDK::UClass* extensions = SDK::UObject::FindClassFast("CommonUIExtensions");
			if (!player || !extensions || !extensions->ClassDefaultObject)
				return nullptr;
			SDK::Params::CommonUIExtensions_PushContentToLayer_ForPlayer parms{};
			parms.LocalPlayer = player;
			parms.LayerName   = layer;
			parms.WidgetClass = cls;
			if (!CallUFunction(extensions->ClassDefaultObject, "CommonUIExtensions", "PushContentToLayer_ForPlayer", &parms, true))
				return nullptr;
			return parms.ReturnValue;
		}

		struct OpenContext
		{
			bool ok = false;
		};

		void OpenPage(void* ctx)
		{
			auto* result = static_cast<OpenContext*>(ctx);
			++g_cycle;
			ModLoaderLogger::LogInfo(L"[NativeSettingsSpike] S3: open #%d", g_cycle);

			const bool full = !g_s1Done;
			g_s1Done = true;
			LogPauseState(L"MODS clicked");

			SDK::UClass* pageClass = FindCustomGameClass();
			if (!pageClass)
				return;
			if (full)
				DumpOptionsTypeWidgets(pageClass);

			SDK::FGameplayTag menuLayer{};
			const bool haveMenuLayer = FindLayer("menu", &menuLayer, full);

			SDK::UCommonActivatableWidget* menu = FindActiveMenu();
			if (!menu)
			{
				ModLoaderLogger::LogWarn(L"[NativeSettingsSpike] S2: no active main or pause menu found -- not opening");
				return;
			}

			SDK::UCommonActivatableWidgetContainerBase* container = FindContainerFor(menu);
			SDK::UCommonActivatableWidget* page = nullptr;
			if (container)
			{
				page = PushToContainer(container, pageClass);
				ModLoaderLogger::LogInfo(L"[NativeSettingsSpike] S2: pushed onto the menu's container -> %S",
				                         NameOf(page).c_str());
			}
			else if (haveMenuLayer)
			{
				page = PushToLayer(menu, menuLayer, pageClass);
				ModLoaderLogger::LogInfo(L"[NativeSettingsSpike] S2: no container holds the menu; pushed to layer %S -> %S",
				                         menuLayer.TagName.ToString().c_str(), NameOf(page).c_str());
			}
			else
			{
				ModLoaderLogger::LogWarn(L"[NativeSettingsSpike] S2: no container and no menu layer -- cannot push the page");
				return;
			}

			if (!page || !page->IsA(SDK::UCrUW_CustomGame::StaticClass()))
			{
				ModLoaderLogger::LogWarn(L"[NativeSettingsSpike] S2: the push returned no Custom Game page");
				return;
			}

			g_page.Set(page);
			g_container.Set(container);
			g_phase     = Phase::Pushed;
			g_phaseTick = g_tick;
			g_statsElapsed = 0.0;
			g_statsFrames  = 0;
			result->ok = true;
		}

		// -------------------------------------------------------------------
		// Build (the tick after activation)
		// -------------------------------------------------------------------
		void WriteOptionData(SDK::FCrCustomGameOptionData& data, SDK::ECrOptionType type,
		                     float minValue, float maxValue, float step, float defaultValue)
		{
			// Plain fields only. The FText fields and RotatorTextOptions are
			// never written: a raw copy of an FText adds an owner without a
			// reference.
			data.Type         = type;
			data.Option       = SDK::ECrCustomGameOption::None;
			data.MinValue     = minValue;
			data.MaxValue     = maxValue;
			data.StepSize     = step;
			data.DefaultValue = defaultValue;
			data.bPercent     = false;
		}

		SDK::UUserWidget* CreateRow(SDK::UCrUW_CustomGame* page, SDK::UClass* cls, const SDK::UClass* expected)
		{
			if (!cls || !expected || !cls->IsSubclassOf(expected))
			{
				ModLoaderLogger::LogWarn(L"[NativeSettingsSpike] S2: row class %S is not a %S -- skipped",
				                         cls ? cls->GetName().c_str() : "<null>", expected ? expected->GetName().c_str() : "<null>");
				return nullptr;
			}
			SDK::UUserWidget* widget = SDK::UWidgetBlueprintLibrary::Create(page, cls, page->GetOwningPlayer());
			if (!widget || !widget->IsA(expected))
			{
				ModLoaderLogger::LogWarn(L"[NativeSettingsSpike] S2: creating %S failed", cls->GetName().c_str());
				return nullptr;
			}
			return widget;
		}

		Row& AddRow(SDK::UWidget* widget, RowKind kind, Mode mode, const char* label, const char* description, size_t padOffset)
		{
			Row row;
			row.ref.Set(widget);
			row.raw         = widget;
			row.kind        = kind;
			row.mode        = mode;
			row.label       = label;
			row.description = description;
			row.padOffset   = padOffset;
			g_rows.push_back(row);   // capacity reserved up front; references stay valid
			return g_rows.back();
		}

		// Finds the first text block inside a widget, through panels and
		// nested user widgets.
		SDK::UTextBlock* FindTextBlock(SDK::UWidget* widget, int depth)
		{
			if (!widget || depth > 8)
				return nullptr;
			if (widget->IsA(SDK::UTextBlock::StaticClass()))
				return static_cast<SDK::UTextBlock*>(widget);
			if (widget->IsA(SDK::UUserWidget::StaticClass()))
			{
				SDK::UWidgetTree* tree = static_cast<SDK::UUserWidget*>(widget)->WidgetTree;
				return tree ? FindTextBlock(tree->RootWidget, depth + 1) : nullptr;
			}
			if (widget->IsA(SDK::UPanelWidget::StaticClass()))
			{
				auto& slots = static_cast<SDK::UPanelWidget*>(widget)->Slots;
				for (int i = 0; i < slots.Num(); ++i)
				{
					SDK::UPanelSlot* slot = slots[i];
					if (SDK::UTextBlock* found = FindTextBlock(slot ? slot->Content : nullptr, depth + 1))
						return found;
				}
			}
			return nullptr;
		}

		void BuildPage(void* ctx)
		{
			auto* built = static_cast<bool*>(ctx);
			auto* page = static_cast<SDK::UCrUW_CustomGame*>(g_page.Get());
			if (!page || !page->OptionsBox)
				return;

			SDK::UScrollBox* box = page->OptionsBox;
			const int stockCount = box->Slots.Num();
			ModLoaderLogger::LogInfo(L"[NativeSettingsSpike] S2: page active; OptionsBox holds %d stock child(ren)", stockCount);
			LogSubsystem(L"before");

			auto* cdo = static_cast<SDK::UCrUW_CustomGame*>(page->Class->ClassDefaultObject);
			if (!cdo)
				return;

			// Hide, don't clear: the container pools this page and may hand the
			// same instance back to the game's own Custom Game screen. The stock
			// rows stay in place and come back when our page closes.
			g_hiddenStock.clear();
			for (int i = 0; i < stockCount; ++i)
			{
				SDK::UPanelSlot* slot = box->Slots[i];
				SDK::UWidget* child = slot ? slot->Content : nullptr;
				if (!child)
					continue;
				HiddenStock hidden;
				hidden.ref.Set(child);
				hidden.visibility = child->Visibility;
				g_hiddenStock.push_back(hidden);
				child->SetVisibility(SDK::ESlateVisibility::Collapsed);
			}

			g_rows.clear();
			g_rows.reserve(kMaxRows);
			g_building = true;

			// Heading, method A: the page's own category line, relabelled.
			{
				const int before = box->Slots.Num();
				SDK::Params::CrUW_CustomGame_AddCategoryLine parms{};
				parms.InCategory = SDK::ECrCustomGameCategory::Building;
				CallUFunction(page, "CrUW_CustomGame", "AddCategoryLine", &parms, false);
				const int after = box->Slots.Num();
				SDK::UWidget* line = (after == before + 1 && box->Slots[after - 1]) ? box->Slots[after - 1]->Content : nullptr;
				SDK::UTextBlock* text = FindTextBlock(line, 0);
				ModLoaderLogger::LogInfo(L"[NativeSettingsSpike] S2: heading A (AddCategoryLine): children %d -> %d, line %S, text block %S",
				                         before, after, ClassNameOf(line).c_str(), NameOf(text).c_str());
				if (line)
				{
					SetTextBlock(text, "Drone panel (F10)");
					AddRow(line, RowKind::CategoryHeading, Mode::None, "Drone panel (F10)", "", 0);
				}
			}

			SDK::APlayerController* owner = page->GetOwningPlayer();
			(void)owner;

			// Toggle, observe mode.
			if (auto* toggle = static_cast<SDK::UCrUW_WidgetOptionToggle*>(
				    CreateRow(page, RowClassFor(cdo, SDK::ECrOptionType::Toggle), SDK::UCrUW_WidgetOptionToggle::StaticClass())))
			{
				WriteOptionData(toggle->OptionData, SDK::ECrOptionType::Toggle, 0.0f, 1.0f, 1.0f, 1.0f);
				Row& row = AddRow(toggle, RowKind::Toggle, Mode::Observe, "Spike toggle (observe)",
				                  "S2 toggle, observe mode: the game's own handler runs. Click ON and OFF.",
				                  offsetof(SDK::UCrUW_WidgetOptionToggle, Pad_448));
				LogRowPad(row, L"after create");
				box->AddChild(toggle);
				SetTextBlock(toggle->OptionTitle, row.label);
				LogRowPad(row, L"after AddChild");
			}

			// Slider, observe mode.
			if (auto* slider = static_cast<SDK::UCrUW_WidgetOptionSlider*>(
				    CreateRow(page, RowClassFor(cdo, SDK::ECrOptionType::Slider), SDK::UCrUW_WidgetOptionSlider::StaticClass())))
			{
				WriteOptionData(slider->OptionData, SDK::ECrOptionType::Slider, 0.0f, 100.0f, 5.0f, 50.0f);
				Row& row = AddRow(slider, RowKind::Slider, Mode::Observe, "Spike slider (observe)",
				                  "S2 slider, observe mode: drag it with the mouse, then step it with a gamepad.",
				                  offsetof(SDK::UCrUW_WidgetOptionSlider, Pad_450));
				LogRowPad(row, L"after create");
				box->AddChild(slider);
				SetTextBlock(slider->OptionTitle, row.label);
				if (slider->Slider)
				{
					slider->Slider->SetMinValue(0.0f);
					slider->Slider->SetMaxValue(100.0f);
					slider->Slider->SetStepSize(5.0f);
					slider->Slider->SetValue(50.0f);
				}
				if (slider->ProgressBar)
					slider->ProgressBar->SetPercent(0.5f);
				SetTextBlock(slider->PercentValue, "50");
				LogRowPad(row, L"after AddChild");
			}

			// Rotator, claim mode from the start: RotatorTextOptions is empty and
			// the stock handler may index it.
			if (auto* rotator = static_cast<SDK::UCrUW_WidgetOptionRotator*>(
				    CreateRow(page, RowClassFor(cdo, SDK::ECrOptionType::Rotator), SDK::UCrUW_WidgetOptionRotator::StaticClass())))
			{
				WriteOptionData(rotator->OptionData, SDK::ECrOptionType::Rotator, 0.0f, 2.0f, 1.0f, 0.0f);
				Row& row = AddRow(rotator, RowKind::Rotator, Mode::Claim, "Spike choice (claim)",
				                  "S2 choice, claim mode: the game's handler is skipped. Step it both ways.",
				                  offsetof(SDK::UCrUW_WidgetOptionRotator, Pad_450));
				LogRowPad(row, L"after create");
				box->AddChild(rotator);
				SetTextBlock(rotator->OptionTitle, row.label);
				if (rotator->OptionTypeRotator)
				{
					SDK::FText labels[3]{};
					const bool haveLabels = MakeText("Alpha", labels[0]) && MakeText("Beta", labels[1]) && MakeText("Gamma", labels[2]);
					if (haveLabels)
					{
						// By-value parameter: ProcessEvent copies it into its own
						// frame and the native body copies again, with references.
						// Our array is only read and never freed by the engine.
						SDK::Params::CommonRotator_PopulateTextLabels populate{};
						populate.Labels = SDK::TArray<SDK::FText>(labels, 3, 3);
						CallUFunction(rotator->OptionTypeRotator, "CommonRotator", "PopulateTextLabels", &populate, true);
						SDK::Params::CommonRotator_SetSelectedItem select{};
						select.InValue = 0;
						CallUFunction(rotator->OptionTypeRotator, "CommonRotator", "SetSelectedItem", &select, true);
					}
				}
				LogRowPad(row, L"after AddChild");
			}

			// Heading, method B: a plain CommonTextBlock of our own.
			{
				SDK::UClass* textClass = SDK::UObject::FindClassFast("CommonTextBlock");
				SDK::UObject* object = (textClass && page->WidgetTree)
					? SDK::UGameplayStatics::SpawnObject(textClass, page->WidgetTree) : nullptr;
				auto* heading = (object && object->IsA(SDK::UTextBlock::StaticClass())) ? static_cast<SDK::UTextBlock*>(object) : nullptr;
				ModLoaderLogger::LogInfo(L"[NativeSettingsSpike] S2: heading B (SpawnObject CommonTextBlock): %S",
				                         NameOf(heading).c_str());
				if (heading)
				{
					SetTextBlock(heading, "Mod Loader settings");
					box->AddChild(heading);
					AddRow(heading, RowKind::TextHeading, Mode::None, "Mod Loader settings", "", 0);
				}
			}

			// S4 key row: a toggle in claim mode; either button opens the
			// "press any key" panel.
			if (auto* keyRow = static_cast<SDK::UCrUW_WidgetOptionToggle*>(
				    CreateRow(page, RowClassFor(cdo, SDK::ECrOptionType::Toggle), SDK::UCrUW_WidgetOptionToggle::StaticClass())))
			{
				WriteOptionData(keyRow->OptionData, SDK::ECrOptionType::Toggle, 0.0f, 1.0f, 1.0f, 0.0f);
				Row& row = AddRow(keyRow, RowKind::KeyCapture, Mode::Claim, "Capture a key (S4)",
				                  "S4: click ON or OFF, then press a key, a mouse button or a gamepad button.",
				                  offsetof(SDK::UCrUW_WidgetOptionToggle, Pad_448));
				box->AddChild(keyRow);
				SetTextBlock(keyRow->OptionTitle, row.label);
			}

			g_building = false;

			// Focus on the first interactive row.
			for (Row& row : g_rows)
			{
				if (row.kind == RowKind::CategoryHeading || row.kind == RowKind::TextHeading)
					continue;
				if (SDK::UWidget* widget = row.ref.Get())
				{
					widget->SetFocus();
					ModLoaderLogger::LogInfo(L"[NativeSettingsSpike] S3: SetFocus on row %d '%S'", RowIndex(&row), row.label);
				}
				break;
			}

			g_childrenAfterBuild = box->Slots.Num();
			ModLoaderLogger::LogInfo(L"[NativeSettingsSpike] S2: built %zu row(s); OptionsBox now holds %d child(ren)",
			                         g_rows.size(), g_childrenAfterBuild);
			*built = true;
		}

		// -------------------------------------------------------------------
		// Checks while the page is open
		// -------------------------------------------------------------------
		void CheckChildCount(const wchar_t* when)
		{
			auto* page = static_cast<SDK::UCrUW_CustomGame*>(g_page.Get());
			if (!page || !page->OptionsBox)
				return;
			const int now = page->OptionsBox->Slots.Num();
			ModLoaderLogger::LogInfo(L"[NativeSettingsSpike] S2: OptionsBox child count %s: %d (after build %d) -- %s",
			                         when, now, g_childrenAfterBuild, now == g_childrenAfterBuild ? L"held" : L"CHANGED");
		}

		void CheckLabelsAndFocus()
		{
			for (Row& row : g_rows)
			{
				SDK::UWidget* widget = row.ref.Get();
				if (!widget)
				{
					ModLoaderLogger::LogInfo(L"[NativeSettingsSpike] S2: row %d '%S' is gone", RowIndex(&row), row.label);
					continue;
				}

				SDK::UTextBlock* title = nullptr;
				switch (row.kind)
				{
				case RowKind::Toggle:
				case RowKind::KeyCapture: title = static_cast<SDK::UCrUW_WidgetOptionToggle*>(widget)->OptionTitle; break;
				case RowKind::Slider:     title = static_cast<SDK::UCrUW_WidgetOptionSlider*>(widget)->OptionTitle; break;
				case RowKind::Rotator:    title = static_cast<SDK::UCrUW_WidgetOptionRotator*>(widget)->OptionTitle; break;
				case RowKind::TextHeading: title = static_cast<SDK::UTextBlock*>(widget); break;
				case RowKind::CategoryHeading: title = FindTextBlock(widget, 0); break;
				}

				const std::wstring text = ReadTextBlock(title);
				const bool focused = widget->HasAnyUserFocus() || widget->HasFocusedDescendants();
				ModLoaderLogger::LogInfo(L"[NativeSettingsSpike] S2: row %d '%S' shows '%s', focus=%d",
				                         RowIndex(&row), row.label, text.c_str(), focused ? 1 : 0);

				// A construct that ran after our SetText would have blanked it.
				if (title && text.empty())
				{
					SetTextBlock(title, row.label);
					ModLoaderLogger::LogInfo(L"[NativeSettingsSpike] S2: row %d label was blank after construct -- set again",
					                         RowIndex(&row));
				}
			}
		}

		void ApplyHover()
		{
			if (g_pendingHover < 0 || g_pendingHover == g_lastHover)
				return;
			g_lastHover = g_pendingHover;
			if (g_lastHover >= static_cast<int>(g_rows.size()))
				return;
			auto* page = static_cast<SDK::UCrUW_CustomGame*>(g_page.Get());
			if (!page || !page->Description)
				return;
			SDK::FText text{};
			if (MakeText(g_rows[g_lastHover].description, text))
			{
				page->Description->SetText(text);
				ModLoaderLogger::LogInfo(L"[NativeSettingsSpike] S2: description pane set for row %d", g_lastHover);
			}
		}

		void DisableRows()
		{
			std::string plugin;
			{
				std::lock_guard<std::mutex> lock(g_forgottenMutex);
				plugin = g_forgottenPlugin;
			}
			if (g_phase != Phase::Built)
			{
				ModLoaderLogger::LogInfo(L"[NativeSettingsSpike] S3: plugin '%S' unloaded while the page was not open",
				                         plugin.c_str());
				return;
			}
			int disabled = 0;
			for (Row& row : g_rows)
			{
				if (SDK::UWidget* widget = row.ref.Get())
				{
					widget->SetIsEnabled(false);
					++disabled;
				}
			}
			ModLoaderLogger::LogInfo(L"[NativeSettingsSpike] S3: plugin '%S' unloaded while the page is open -- rows disabled (%d)",
			                         plugin.c_str(), disabled);
		}

		void ReportTickStats(float deltaSeconds)
		{
			if (g_statsFrames == 0)
			{
				g_statsMin = g_statsMax = deltaSeconds;
			}
			g_statsMin = (deltaSeconds < g_statsMin) ? deltaSeconds : g_statsMin;
			g_statsMax = (deltaSeconds > g_statsMax) ? deltaSeconds : g_statsMax;
			g_statsElapsed += deltaSeconds;
			++g_statsFrames;
			if (g_statsElapsed < kStatsPeriodSec)
				return;
			SDK::UWorld* world = SDK::UWorld::GetWorld();
			const bool paused = world && SDK::UGameplayStatics::IsGamePaused(world);
			ModLoaderLogger::LogInfo(L"[NativeSettingsSpike] S1: tick while open: %d frames in %.2f s, delta min %.4f max %.4f, IsGamePaused=%d",
			                         g_statsFrames, g_statsElapsed, g_statsMin, g_statsMax, paused ? 1 : 0);
			g_statsElapsed = 0.0;
			g_statsFrames  = 0;
		}

		// -------------------------------------------------------------------
		// S4
		// -------------------------------------------------------------------
		SDK::UClass* FindPressAnyKeyClass()
		{
			const SDK::UClass* native  = SDK::UGameSettingPressAnyKey::StaticClass();
			const SDK::UClass* warning = SDK::UKeyAlreadyBoundWarning::StaticClass();
			for (SDK::UClass* cls : FindLoadedSubclasses(native))
			{
				if (warning && cls->IsSubclassOf(warning))
					continue;
				ModLoaderLogger::LogInfo(L"[NativeSettingsSpike] S4: press-any-key class (loaded): %S", cls->GetFullName().c_str());
				return cls;
			}

			for (SDK::UClass* entry : FindLoadedSubclasses(SDK::UCrUW_SettingsListEntrySetting_KeyboardInput::StaticClass()))
			{
				auto* cdo = static_cast<SDK::UCrUW_SettingsListEntrySetting_KeyboardInput*>(entry->ClassDefaultObject);
				SDK::UClass* cls = cdo ? cdo->PressAnyKeyPanelClass.Get() : nullptr;
				if (cls)
				{
					ModLoaderLogger::LogInfo(L"[NativeSettingsSpike] S4: press-any-key class (from %S): %S",
					                         entry->GetName().c_str(), cls->GetFullName().c_str());
					return cls;
				}
			}

			ModLoaderLogger::LogWarn(L"[NativeSettingsSpike] S4: no press-any-key class is loaded. Open OPTIONS -> "
			                         L"keyboard controls once, back out, then try the key row again.");
			return nullptr;
		}

		void OpenKeyPanel(void* ctx)
		{
			(void)ctx;
			SDK::UClass* cls = FindPressAnyKeyClass();
			if (!cls)
				return;

			SDK::UCommonActivatableWidget* panel = nullptr;
			SDK::FGameplayTag modal{};
			if (FindLayer("modal", &modal, false))
			{
				panel = PushToLayer(g_page.Get(), modal, cls);
				ModLoaderLogger::LogInfo(L"[NativeSettingsSpike] S4: pushed to layer %S -> %S",
				                         modal.TagName.ToString().c_str(), NameOf(panel).c_str());
			}
			if (!panel)
			{
				if (SDK::UCommonActivatableWidgetContainerBase* container = g_container.Get())
				{
					panel = PushToContainer(container, cls);
					ModLoaderLogger::LogInfo(L"[NativeSettingsSpike] S4: pushed onto the page's container -> %S",
					                         NameOf(panel).c_str());
				}
			}
			if (!panel || !panel->IsA(SDK::UGameSettingPressAnyKey::StaticClass()))
			{
				ModLoaderLogger::LogWarn(L"[NativeSettingsSpike] S4: the push returned no press-any-key panel");
				return;
			}
			g_keyPanel.Set(panel);
			g_lastKeyText[0] = L'\0';
		}

		void WatchKeyPanel()
		{
			auto* panel = static_cast<SDK::UGameSettingPressAnyKey*>(g_keyPanel.Get());
			if (!panel || !panel->bIsActive)
			{
				ModLoaderLogger::LogInfo(L"[NativeSettingsSpike] S4: key panel closed; last key text '%s'", g_lastKeyText);
				g_keyPanel.Reset();
				return;
			}
			wchar_t text[128]{};
			if (panel->KeyText)
				ReadTextGuarded(&panel->KeyText->Text, text, _countof(text));
			if (wcscmp(text, g_lastKeyText) != 0)
			{
				wcscpy_s(g_lastKeyText, text);
				ModLoaderLogger::LogInfo(L"[NativeSettingsSpike] S4: key text now '%s'", text);
			}
		}

		// -------------------------------------------------------------------
		// Close
		// -------------------------------------------------------------------
		void ClosePage(const wchar_t* why)
		{
			int removed = 0;
			int restored = 0;
			auto* page = static_cast<SDK::UCrUW_CustomGame*>(g_page.Get());
			SDK::UScrollBox* box = page ? page->OptionsBox : nullptr;
			if (box)
			{
				for (Row& row : g_rows)
				{
					if (SDK::UWidget* widget = row.ref.Get())
					{
						if (box->RemoveChild(widget))
							++removed;
					}
				}
				for (HiddenStock& hidden : g_hiddenStock)
				{
					if (SDK::UWidget* widget = hidden.ref.Get())
					{
						widget->SetVisibility(hidden.visibility);
						++restored;
					}
				}
			}

			for (const Row& row : g_rows)
			{
				if (row.padOffset)
					ModLoaderLogger::LogInfo(L"[NativeSettingsSpike] S2: row %d '%S' saw %d handler event(s)",
					                         RowIndex(&row), row.label, row.events);
			}

			ModLoaderLogger::LogInfo(L"[NativeSettingsSpike] S3: %s -- page gone, rows dropped (%d removed, %d stock row(s) restored)",
			                         why, removed, restored);
			LogSubsystem(L"after");

			g_rows.clear();
			g_hiddenStock.clear();
			g_page.Reset();
			g_container.Reset();
			g_keyPanel.Reset();
			g_phase       = Phase::Idle;
			g_lastHover   = -1;
			g_pendingHover = -1;
			g_childrenAfterBuild = -1;
		}

		void Guarded(GuardedFn fn, void* ctx, const wchar_t* what)
		{
			unsigned long code = 0;
			if (RunGuarded(fn, ctx, &code))
				return;
			g_broken = true;
			g_building = false;
			ModLoaderLogger::LogError(L"[NativeSettingsSpike] exception 0x%08lX while %s -- the spike stops here. "
			                          L"Back out of the page and quit to desktop.", code, what);
		}

		struct TickContext
		{
			float deltaSeconds;
		};

		void TickBody(void* ctx)
		{
			const float deltaSeconds = static_cast<TickContext*>(ctx)->deltaSeconds;

			if (g_disableRequested.exchange(false))
				DisableRows();

			if (g_openRequested.exchange(false))
			{
				if (g_phase != Phase::Idle)
				{
					ModLoaderLogger::LogInfo(L"[NativeSettingsSpike] S3: MODS clicked while the page is already open -- ignored");
				}
				else if (!IsMainMenuWorld())
				{
					ModLoaderLogger::LogWarn(L"[NativeSettingsSpike] the spike opens at the main menu only -- "
					                         L"quit to the title screen first");
				}
				else
				{
					OpenContext open;
					Guarded(&OpenPage, &open, L"opening the page");
				}
			}

			if (g_phase == Phase::Pushed)
			{
				auto* page = g_page.Get();
				if (!page)
				{
					ModLoaderLogger::LogWarn(L"[NativeSettingsSpike] S2: the pushed page disappeared before it activated");
					ClosePage(L"page vanished");
				}
				else if (IsActivated(page) && g_tick > g_phaseTick)
				{
					bool built = false;
					Guarded(&BuildPage, &built, L"building the page");
					if (built)
					{
						g_phase     = Phase::Built;
						g_phaseTick = g_tick;
					}
				}
				else if (g_tick - g_phaseTick > kActivateTimeout)
				{
					ModLoaderLogger::LogWarn(L"[NativeSettingsSpike] S2: the page did not activate within %d ticks", kActivateTimeout);
					ClosePage(L"activation timed out");
				}
				return;
			}

			if (g_phase != Phase::Built)
				return;

			auto* page = g_page.Get();
			if (!page)
			{
				ClosePage(L"page object gone");
				return;
			}
			if (!IsActivated(page))
			{
				ClosePage(L"page deactivated (back, Escape, B or menu closed)");
				return;
			}

			ReportTickStats(deltaSeconds);

			const uint64_t since = g_tick - g_phaseTick;
			if (since == 1)
			{
				CheckChildCount(L"on the next tick");
				CheckLabelsAndFocus();
			}
			else if (since == kRecheckTicks)
			{
				CheckChildCount(L"half a second later");
				CheckLabelsAndFocus();
			}

			ApplyHover();

			if (g_keyCaptureRequested.exchange(false) && !g_keyPanel.Get())
				OpenKeyPanel(nullptr);
			if (g_keyPanel.Get() || g_lastKeyText[0])
			{
				if (g_keyPanel.Get())
					WatchKeyPanel();
				else
					g_lastKeyText[0] = L'\0';
			}
		}
	}

	// -----------------------------------------------------------------------
	// Public
	// -----------------------------------------------------------------------
	bool Install()
	{
		if (g_installed)
			return true;

		ModLoaderLogger::LogInfo(L"[NativeSettingsSpike] Installing (throwaway P0 build)");
		ResolveHandlers();

		int thunks = 0;
		int members = 0;
		for (int i = 0; i < kHandlerCount; ++i)
		{
			Handler& h = g_handlers[i];
			if (h.exec && h.thunkHook.Install(h.exec, kThunkDetours[i], &h.thunkOrig, "NativeSettingsSpike", h.funcName))
				++thunks;
			if (h.member && h.memberHook.Install(h.member, kMemberDetours[i], &h.memberOrig, "NativeSettingsSpike", h.funcName))
				++members;
		}

		SDK::UClass* baseOption = SDK::UCrUW_BaseOption::StaticClass();
		SDK::UFunction* hover = baseOption ? baseOption->GetFunction("CrUW_BaseOption", "SetHoverVisuals") : nullptr;
		if (hover)
		{
			g_hoverEventName = hover->Name;
			Hooks::GameInstanceInit::RegisterProcessEventCallback(&HoverObserver);
			g_hoverObserving = true;
		}

		ModLoaderLogger::LogInfo(L"[NativeSettingsSpike] S2: %d of %d exec thunks hooked, %d of %d members hooked, hover %s",
		                         thunks, kHandlerCount, members, kHandlerCount, g_hoverObserving ? L"observed" : L"NOT observed");
		g_installed = true;
		return true;
	}

	void Remove()
	{
		if (!g_installed)
			return;
		if (g_hoverObserving)
			Hooks::GameInstanceInit::UnregisterProcessEventCallback(&HoverObserver);
		g_hoverObserving = false;
		for (Handler& h : g_handlers)
		{
			if (h.memberHook.installed)
				h.memberHook.Remove();
			if (h.thunkHook.installed)
				h.thunkHook.Remove();
			h.memberOrig = nullptr;
			h.thunkOrig  = nullptr;
		}
		g_installed = false;
	}

	bool IsInstalled()
	{
		return g_installed;
	}

	void RequestOpen()
	{
		g_openRequested.store(true);
	}

	void ForgetPlugin(const char* pluginName)
	{
		{
			std::lock_guard<std::mutex> lock(g_forgottenMutex);
			g_forgottenPlugin = pluginName ? pluginName : "";
		}
		g_disableRequested.store(true);
	}

	void Tick(float deltaSeconds)
	{
		++g_tick;
		if (g_broken)
			return;
		TickContext ctx{ deltaSeconds };
		Guarded(&TickBody, &ctx, L"ticking");
	}
}

#endif // MODLOADER_CLIENT_BUILD

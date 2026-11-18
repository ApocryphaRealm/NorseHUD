// NorseHUD - bars over units, a boss bar and the lock-on marker (UE4SS v4.0.0 C++ mod for NORSE: Oath of Blood),
// 2026-11-17. Modelled on TrueHUD (Skyrim): one shared on-screen layer the overhaul's modules use instead of each
// drawing its own.
//
// How it draws: Norse's HUD class overrides AHUD::DrawHUD and never fires the ReceiveDrawHUD Blueprint event (first
// test: 0 calls in 2 s), so NorseHUD hooks DrawHUD itself. AHUD::DrawHUD is Norse Address Library ID 10 (no reflected
// name - found by signature; identified as the one function reading the ReceiveDrawHUD name). Its slot in the engine
// HUD's vtable is looked up at run time, and the function the live HUD class has in that slot (Norse's override) is
// hooked; after it returns, the HUD's Canvas is still valid and NorseHUD calls the HUD's own Project / DrawRect /
// DrawText - Unreal's canvas, nothing cooked, nothing in the paks changes.
// What is drawn - NorseHUD.ini:
//   a health bar over every unit for which sBarCondition holds (NorseConditions; default "InFight AND NOT IsDead"),
//   red for enemies, green for the player's side; a boss bar at the top for a living boss unit (its unit type's
//   bIsBoss) in the fight; a marker over NorseRealtimeNative's lock-on target.
// Cost: the unit list, team, boss flag, name and the condition are refreshed once a second; per frame, per shown unit:
// its location, one Project and two or three DrawRects.
#include "NorseReflect.hpp"
#include "NorseAddressLibrary.hpp"

#include <MinHook.h>

#include <cmath>
#include <map>

namespace fs = std::filesystem;
using namespace nrn;
using namespace nrn::ue;
using namespace norse;

namespace
{
    struct Config
    {
        bool Bars = true, BossBar = true, LockMarker = true;
        std::wstring BarCondition = L"InFight AND NOT IsDead";
        float BarWidth = 80, BarHeight = 7, BarZ = 115, MaxDistance = 3500;
        std::wstring LogDir;
    } g_cfg;
    fs::path g_mod;

    fs::path ModDir()
    {
        HMODULE self = nullptr;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(&ModDir), &self);
        wchar_t buf[MAX_PATH * 2]{};
        GetModuleFileNameW(self, buf, static_cast<DWORD>(std::size(buf)));
        return fs::path(buf).parent_path().parent_path();
    }

    void LoadConfig()
    {
        g_cfg = Config{};
        for (const auto& raw : ReadLines(g_mod / "NorseHUD.ini"))
        {
            std::wstring l = Trim(StripComment(raw));
            size_t eq = l.find(L'=');
            if (l.empty() || l[0] == L'[' || eq == std::wstring::npos) continue;
            std::wstring k = Trim(l.substr(0, eq)), v = Trim(l.substr(eq + 1));
            if (k == L"bBars") g_cfg.Bars = _wtoi(v.c_str()) != 0;
            else if (k == L"bBossBar") g_cfg.BossBar = _wtoi(v.c_str()) != 0;
            else if (k == L"bLockMarker") g_cfg.LockMarker = _wtoi(v.c_str()) != 0;
            else if (k == L"sBarCondition") g_cfg.BarCondition = v;
            else if (k == L"fBarWidth") g_cfg.BarWidth = static_cast<float>(_wtof(v.c_str()));
            else if (k == L"fBarHeight") g_cfg.BarHeight = static_cast<float>(_wtof(v.c_str()));
            else if (k == L"fBarZ") g_cfg.BarZ = static_cast<float>(_wtof(v.c_str()));
            else if (k == L"fMaxDistance") g_cfg.MaxDistance = static_cast<float>(_wtof(v.c_str()));
            else if (k == L"sLogDir") g_cfg.LogDir = v;
        }
    }

    // ------------------------------------------------------------------ other mods' exports
    template <typename F>
    F Export(const wchar_t* mod, const char* name)
    {
        fs::path dll = g_mod.parent_path() / mod / L"dlls" / L"main.dll";
        HMODULE m = GetModuleHandleW(dll.c_str());
        return m ? reinterpret_cast<F>(GetProcAddress(m, name)) : nullptr;
    }
    using NcCompileFn = int (*)(const char*, char*, int);
    using NcEvalFn = int (*)(int, void*, void*);
    using NrnObjFn = void* (*)();
    NcEvalFn g_ncEval = nullptr;
    int g_barCond = 0;
    NrnObjFn g_lockTarget = nullptr, g_hero = nullptr;

    void FindExports()
    {
        if (!g_ncEval)
        {
            auto compile = Export<NcCompileFn>(L"NorseConditions", "NC_Compile");
            g_ncEval = Export<NcEvalFn>(L"NorseConditions", "NC_Eval");
            if (compile && g_ncEval && !g_cfg.BarCondition.empty())
            {
                char err[256]{};
                g_barCond = compile(Narrow(g_cfg.BarCondition).c_str(), err, sizeof(err));
                if (g_barCond) NRN_INFO("bars shown when: %s (NorseConditions)", Narrow(g_cfg.BarCondition).c_str());
                else NRN_ERROR("sBarCondition does not compile: %s - bars shown for every living unit", err);
            }
        }
        if (!g_lockTarget) g_lockTarget = Export<NrnObjFn>(L"NorseRealtimeNative", "NRN_LockTarget");
        if (!g_hero) g_hero = Export<NrnObjFn>(L"NorseRealtimeNative", "NRN_Hero");
    }

    // ------------------------------------------------------------------ units (refreshed once a second)
    struct Unit
    {
        UObject* actor = nullptr;
        UObject* comp = nullptr; // UnitComponent
        int32_t actorIx = -1, compIx = -1; // checked with Alive() before each use: a unit can be destroyed between refreshes
        int maxHp = 0;
        bool enemy = false, boss = false, show = false;
        std::string name;
    };
    std::vector<Unit> g_units;
    UObject* g_teamLib = nullptr;
    double g_nextRefresh = 0;

    std::string UnitName(UObject* type)
    {
        auto* cls = reinterpret_cast<UStruct*>(Reflect::Get().ClassOf(type));
        FProperty* p = cls ? FindProp(cls, L"Name") : nullptr;
        std::wstring t = p ? norse::Export(p, reinterpret_cast<uint8_t*>(type) + p->GetOffset_Internal(), type) : L"";
        // NSLOCTEXT("ns", "key", "Display") -> Display
        size_t b = t.rfind(L'"'), a = b == std::wstring::npos ? b : t.rfind(L'"', b - 1);
        if (a != std::wstring::npos && b > a) return Narrow(t.substr(a + 1, b - a - 1));
        return Narrow(ShortName(type->GetFullName(nullptr)));
    }

    bool FieldTrue(UObject* obj, const wchar_t* field)
    {
        auto* cls = reinterpret_cast<UStruct*>(Reflect::Get().ClassOf(obj));
        FProperty* p = cls ? FindProp(cls, field) : nullptr;
        return p && Lower(norse::Export(p, reinterpret_cast<uint8_t*>(obj) + p->GetOffset_Internal(), obj)) == L"true";
    }

    void Refresh()
    {
        FindExports();
        std::vector<UObject*> actors;
        try
        {
            RC::Unreal::UObjectGlobals::FindAllOf(L"BP_Metahuman_Unit_C", actors);
        }
        catch (...)
        {
        }
        if (!g_teamLib)
            g_teamLib = RC::Unreal::UObjectGlobals::StaticFindObject_InternalSlow(nullptr, nullptr, L"/Script/Norse.Default__TeamIDLibrary", false);
        std::vector<Unit> out;
        for (UObject* a : actors)
        {
            if (!Alive(a, IndexOf(a))) continue; // destroyed, waiting for the garbage collector
            Unit u;
            u.actor = a;
            u.actorIx = IndexOf(a);
            auto** uc = Reflect::Get().Prop<UObject*>(a, L"UnitComponent");
            u.comp = uc ? *uc : nullptr;
            if (!u.comp || !Alive(u.comp, IndexOf(u.comp))) continue;
            u.compIx = IndexOf(u.comp);
            Buf p;
            if (Reflect::Get().Call(u.comp, L"GetMaxHealth", p.b, 4, sizeof(p.b))) std::memcpy(&u.maxHp, p.b, 4);
            uint8_t team = 255;
            if (g_teamLib) GetTeamIDSafe(g_teamLib, a, team);
            u.enemy = team != 0;
            auto** type = Reflect::Get().Prop<UObject*>(u.comp, L"CombatUnit");
            if (type && *type)
            {
                u.boss = FieldTrue(*type, L"bIsBoss");
                u.name = UnitName(*type);
            }
            u.show = g_barCond && g_ncEval ? g_ncEval(g_barCond, a, nullptr) != 0 : true;
            out.push_back(u);
        }
        g_units.swap(out);
    }

    int Health(const Unit& u)
    {
        auto* hp = Reflect::Get().Prop<int32_t>(u.comp, L"RemainingHealth");
        return hp ? *hp : 0;
    }

    // ------------------------------------------------------------------ drawing (inside ReceiveDrawHUD)
    struct Color
    {
        float r, g, b, a;
    };

    bool g_drawOk = true;

    void Rect(UObject* hud, const Color& c, float x, float y, float w, float h)
    {
        Buf p; // AHUD::DrawRect(FLinearColor RectColor @0, float ScreenX @16, ScreenY @20, ScreenW @24, ScreenH @28)
        std::memcpy(p.b, &c, 16);
        float v[4] = {x, y, w, h};
        std::memcpy(p.b + 16, v, 16);
        if (!Reflect::Get().Call(hud, L"DrawRect", p.b, 32, sizeof(p.b))) g_drawOk = false;
    }

    void Text(UObject* hud, const std::wstring& s, const Color& c, float x, float y, float scale)
    {
        Buf p; // AHUD::DrawText(FString Text @0, FLinearColor @16, X @32, Y @36, UFont* @40, Scale @48, bScalePosition @52)
        struct FStr
        {
            const wchar_t* data;
            int32_t num, max;
        } str{s.c_str(), static_cast<int32_t>(s.size() + 1), static_cast<int32_t>(s.size() + 1)};
        std::memcpy(p.b, &str, 16);
        std::memcpy(p.b + 16, &c, 16);
        std::memcpy(p.b + 32, &x, 4);
        std::memcpy(p.b + 36, &y, 4);
        std::memcpy(p.b + 48, &scale, 4);
        Reflect::Get().Call(hud, L"DrawText", p.b, 53, sizeof(p.b));
    }

    // GameplayStatics::ProjectWorldToScreen(Player @0, WorldPosition @8, ScreenPosition& @0x20, bPlayerViewportRelative
    // @0x30) -> bool @0x31 - the player's own camera (as NorseRealtimeNative's lock marker). First test: AHUD::Project used
    // the canvas's view, which is not the follow camera NorseRealtimeNative drives: left-right right, up-down wrong, and
    // the bar height setting changed nothing.
    bool Project(UObject* hud, const FVec& w, FVec& screen)
    {
        static UObject* gs = nullptr;
        if (!gs) gs = RC::Unreal::UObjectGlobals::StaticFindObject_InternalSlow(nullptr, nullptr, L"/Script/Engine.Default__GameplayStatics", false);
        auto** pc = Reflect::Get().Prop<UObject*>(hud, L"PlayerOwner");
        if (!gs || !pc || !*pc) return false;
        Buf p;
        std::memcpy(p.b, pc, 8);
        std::memcpy(p.b + 8, &w, 24);
        p.b[0x30] = 0;
        if (!Reflect::Get().Call(gs, L"ProjectWorldToScreen", p.b, 0x32, sizeof(p.b)) || !p.b[0x31]) return false;
        double sp[2] = {0, 0};
        std::memcpy(sp, p.b + 0x20, 16);
        screen = FVec{sp[0], sp[1], 1};
        return true;
    }

    void Bar(UObject* hud, float cx, float y, float w, float h, float frac, const Color& fill)
    {
        frac = std::clamp(frac, 0.f, 1.f);
        Rect(hud, {0, 0, 0, 0.6f}, cx - w / 2 - 1, y - 1, w + 2, h + 2);
        Rect(hud, fill, cx - w / 2, y, w * frac, h);
    }

    uint64_t g_frames = 0, g_drawn = 0;

    void Draw(UObject* hud)
    {
        int32_t sx = 0, sy = 0;
        auto** canvas = Reflect::Get().Prop<UObject*>(hud, L"Canvas");
        if (canvas && *canvas)
        {
            if (auto* x = Reflect::Get().Prop<int32_t>(*canvas, L"SizeX")) sx = *x;
            if (auto* y = Reflect::Get().Prop<int32_t>(*canvas, L"SizeY")) sy = *y;
        }

        double now = GetTickCount64() / 1000.0;
        if (now >= g_nextRefresh)
        {
            Refresh();
            g_nextRefresh = now + 1.0;
        }
        ++g_frames;
        UObject* hero = g_hero ? static_cast<UObject*>(g_hero()) : nullptr;
        FVec heroAt{};
        bool haveHero = hero && GetVec(hero, L"K2_GetActorLocation", heroAt);
        UObject* target = g_cfg.LockMarker && g_lockTarget ? static_cast<UObject*>(g_lockTarget()) : nullptr;
        const Unit* boss = nullptr;
        for (const Unit& u : g_units)
        {
            if (!Alive(u.actor, u.actorIx) || !Alive(u.comp, u.compIx)) continue;
            int hp = Health(u);
            if (u.boss && hp > 0 && u.show && !boss) boss = &u;
            if (!(g_cfg.Bars && u.show) && u.actor != target) continue;
            FVec at{};
            if (!GetVec(u.actor, L"K2_GetActorLocation", at)) continue;
            if (haveHero)
            {
                double dx = at.X - heroAt.X, dy = at.Y - heroAt.Y;
                if (std::sqrt(dx * dx + dy * dy) > g_cfg.MaxDistance) continue;
            }
            at.Z += g_cfg.BarZ;
            FVec s{};
            if (!Project(hud, at, s)) continue;
            float x = static_cast<float>(s.X), y = static_cast<float>(s.Y);
            static double lastWhere = 0;
            if (now - lastWhere > 2.0)
            {
                lastWhere = now;
                NRN_INFO("bar check: %s world (%.0f, %.0f, %.0f) incl. +%.0f -> screen (%.0f, %.0f), canvas %dx%d", Name(u.actor).c_str(), at.X, at.Y,
                         at.Z, g_cfg.BarZ, s.X, s.Y, sx, sy);
            }
            if (g_cfg.Bars && u.show && u.maxHp > 0)
            {
                Bar(hud, x, y, g_cfg.BarWidth, g_cfg.BarHeight, static_cast<float>(hp) / u.maxHp,
                    u.enemy ? Color{0.75f, 0.08f, 0.06f, 0.9f} : Color{0.15f, 0.65f, 0.2f, 0.9f});
                ++g_drawn;
            }
            if (u.actor == target) // a gold diamond-ish marker above the bar
            {
                Rect(hud, {0.95f, 0.75f, 0.2f, 0.95f}, x - 5, y - 16, 10, 10);
                Rect(hud, {0, 0, 0, 0.8f}, x - 2, y - 13, 4, 4);
            }
        }
        if (g_cfg.BossBar && boss && sx > 0)
        {
            float w = sx * 0.4f;
            Bar(hud, sx / 2.f, 40.f, w, 12.f, static_cast<float>(Health(*boss)) / std::max(1, boss->maxHp), {0.7f, 0.05f, 0.05f, 0.95f});
            Text(hud, Widen(boss->name), {0.95f, 0.9f, 0.8f, 1}, sx / 2.f - w / 2, 18.f, 1.2f);
        }
        if (g_frames == 1 || g_frames % 600 == 0)
            NRN_INFO("drawing: frame %llu, %zu unit(s) known, %llu bar(s) drawn so far%s", static_cast<unsigned long long>(g_frames), g_units.size(),
                     static_cast<unsigned long long>(g_drawn), g_drawOk ? "" : " - DrawRect calls FAILED (see the warnings)");
    }

    // ------------------------------------------------------------------ the DrawHUD hook
    using DrawHudFn = void(__fastcall*)(UObject* hud);
    std::map<void*, DrawHudFn> g_drawOriginals; // hooked function -> trampoline
    size_t g_drawSlot = 0;
    bool g_hudOff = false;

    void __fastcall DrawHudHook(UObject* hud)
    {
        void* fn = (*reinterpret_cast<void***>(hud))[g_drawSlot];
        if (auto it = g_drawOriginals.find(fn); it != g_drawOriginals.end()) it->second(hud);
        if (g_hudOff) return;
        try
        {
            Draw(hud);
        }
        catch (...)
        {
            NRN_ERROR("drawing threw - NorseHUD drawing off");
            g_hudOff = true;
        }
    }

    void HookHud()
    {
        UObject* hud = nullptr;
        try
        {
            hud = RC::Unreal::UObjectGlobals::FindFirstOf(L"HUD");
        }
        catch (...)
        {
        }
        if (!hud) return;
        if (!g_drawSlot)
        {
            static bool nalTried = false;
            if (!nalTried)
            {
                nalTried = true;
                fs::path nalDir = g_mod.parent_path().parent_path() / "NorseAddressLibrary";
                nal::Library::Get().Load(nalDir, [](const std::string& m) { NRN_INFO("%s", m.c_str()); });
            }
            void* engineDraw = reinterpret_cast<void*>(nal::Library::Get().Address(nal::ID::AHUD::DrawHUD));
            UObject* base = RC::Unreal::UObjectGlobals::StaticFindObject_InternalSlow(nullptr, nullptr, L"/Script/Engine.Default__HUD", false);
            if (!engineDraw || !base)
            {
                NRN_ERROR("AHUD::DrawHUD (NAL ID 10) %s, Default__HUD %s - no HUD drawing", engineDraw ? "found" : "NOT found",
                          base ? "found" : "NOT found");
                g_hudOff = true;
                return;
            }
            void** vt = *reinterpret_cast<void***>(base);
            for (size_t i = 1; i < 600 && !g_drawSlot; ++i)
                if (vt[i] == engineDraw) g_drawSlot = i;
            if (!g_drawSlot)
            {
                NRN_ERROR("AHUD::DrawHUD is in no slot of the engine HUD's vtable - no HUD drawing");
                g_hudOff = true;
                return;
            }
            NRN_INFO("AHUD::DrawHUD is vtable slot %zu", g_drawSlot);
        }
        void* fn = (*reinterpret_cast<void***>(hud))[g_drawSlot];
        if (g_drawOriginals.count(fn)) return;
        DrawHudFn orig = nullptr;
        MH_STATUS st = MH_Initialize();
        (void)st;
        if (MH_CreateHook(fn, reinterpret_cast<void*>(&DrawHudHook), reinterpret_cast<void**>(&orig)) != MH_OK || MH_EnableHook(fn) != MH_OK)
        {
            NRN_ERROR("DrawHUD of %s: hook failed", Name(hud).c_str());
            return;
        }
        g_drawOriginals[fn] = orig;
        uintptr_t mod = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
        NRN_INFO("HUD %s: DrawHUD (slot %zu) hooked at base+0x%llx; DrawRect %u, DrawText %u, Project %u bytes of parameters", Name(hud).c_str(),
                 g_drawSlot, static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(fn) - mod), Reflect::Get().ParmsSizeOf(hud, L"DrawRect"),
                 Reflect::Get().ParmsSizeOf(hud, L"DrawText"), Reflect::Get().ParmsSizeOf(hud, L"Project"));
    }
} // namespace

class NorseHUD : public RC::CppUserModBase
{
  public:
    NorseHUD()
    {
        ModName = L"NorseHUD";
        ModVersion = L"" NH_VERSION;
        ModDescription = L"Bars over units, a boss bar and the lock-on marker, drawn on the game's HUD canvas.";
        ModAuthors = L"ApocryphaRealm";
        for (auto& s : g_nopVtbl) s = reinterpret_cast<void*>(&NopFn);
        g_errOut.vtbl = g_nopVtbl;
        g_mod = ModDir();
        LoadConfig();
        Log::Get().Open((g_cfg.LogDir.empty() ? g_mod : fs::path(g_cfg.LogDir)) / "NorseHUD.log", 2);
        Log::Get().Write(Level::Error, "NorseHUD v%s - bars %d, boss bar %d, lock marker %d", NH_VERSION, g_cfg.Bars, g_cfg.BossBar, g_cfg.LockMarker);
    }

    auto on_unreal_init() -> void override
    {
        try
        {
            RC::Unreal::Hook::RegisterInitGameStatePostCallback([](RC::Unreal::AGameModeBase*) {
                g_units.clear();
                g_nextRefresh = 0;
                HookHud();
            });
            RC::Unreal::Hook::RegisterEngineTickPreCallback([](RC::Unreal::UEngine*, float) {
                static int n = 0;
                if (++n % 120 == 0) HookHud(); // a HUD class first seen mid-map (cheap: one FindFirstOf every ~2 s)
            });
            NRN_INFO("callbacks registered");
        }
        catch (const std::exception& e)
        {
            NRN_ERROR("callback registration failed: %s", e.what());
        }
    }
};

extern "C" __declspec(dllexport) RC::CppUserModBase* start_mod()
{
    return new NorseHUD();
}

extern "C" __declspec(dllexport) void uninstall_mod(RC::CppUserModBase* mod)
{
    delete mod;
}

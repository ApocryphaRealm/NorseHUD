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
//   the ability strip in real-time fights (2026-11-20): NorseRealtimeNative's LT + LB / X / Y / A / B abilities with their
//   icons, cooldowns and stamina state (NRN_GetAbilitySlots); the game's turn-based ability bar is collapsed meanwhile;
//   a health bar over every unit for which sBarCondition holds (NorseConditions; default "InFight AND NOT IsDead"),
//   red for enemies, green for the player's side; a boss bar at the top for a living boss unit (its unit type's
//   bIsBoss) in the fight; a marker over NorseRealtimeNative's lock-on target; a compass at the top with the current
//   quest objective's bearing (2026-11-22, bCompass).
// Console: nhud.status (settings, hook and unit counts to the log), nhud.set <key> <value> (one NorseHUD.ini key for this
// session), nhud.reload (reads NorseHUD.ini again).
// Cost: the unit list, team, boss flag, name and the condition are refreshed once a second; per frame, per shown unit:
// its location, one Project and two or three DrawRects.
#include "NorseReflect.hpp"
#include "NorseAddressLibrary.hpp"

#include <MinHook.h>

#include <climits>
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
        bool Bars = true, BossBar = true, LockMarker = true, AbilityStrip = true, Compass = true;
        float CompassWidth = 560, CompassTop = 14;
        float StripSize = 64, StripBottom = 70;
        std::wstring BarCondition = L"InFight AND NOT IsDead";
        float BarWidth = 80, BarHeight = 7, BarZ = 115, MaxDistance = 3500;
        int LogLevel = 2; // uLogLevel: 0 trace, 1 debug, 2 info, 3 warn, 4 error, 6 off
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

    // One INI key; false when the key is not one of NorseHUD's. Shared by LoadConfig and nhud.set.
    bool ApplyKey(const std::wstring& k, const std::wstring& v)
    {
        if (k == L"bBars") g_cfg.Bars = _wtoi(v.c_str()) != 0;
        else if (k == L"bBossBar") g_cfg.BossBar = _wtoi(v.c_str()) != 0;
        else if (k == L"bLockMarker") g_cfg.LockMarker = _wtoi(v.c_str()) != 0;
        else if (k == L"sBarCondition") g_cfg.BarCondition = v;
        else if (k == L"fBarWidth") g_cfg.BarWidth = static_cast<float>(_wtof(v.c_str()));
        else if (k == L"fBarHeight") g_cfg.BarHeight = static_cast<float>(_wtof(v.c_str()));
        else if (k == L"fBarZ") g_cfg.BarZ = static_cast<float>(_wtof(v.c_str()));
        else if (k == L"fMaxDistance") g_cfg.MaxDistance = static_cast<float>(_wtof(v.c_str()));
        else if (k == L"bAbilityStrip") g_cfg.AbilityStrip = _wtoi(v.c_str()) != 0;
        else if (k == L"fStripSize") g_cfg.StripSize = std::clamp(static_cast<float>(_wtof(v.c_str())), 32.f, 128.f);
        else if (k == L"fStripBottom") g_cfg.StripBottom = std::clamp(static_cast<float>(_wtof(v.c_str())), 0.f, 600.f);
        else if (k == L"bCompass") g_cfg.Compass = _wtoi(v.c_str()) != 0;
        else if (k == L"fCompassWidth") g_cfg.CompassWidth = std::clamp(static_cast<float>(_wtof(v.c_str())), 200.f, 1600.f);
        else if (k == L"fCompassTop") g_cfg.CompassTop = std::clamp(static_cast<float>(_wtof(v.c_str())), 0.f, 600.f);
        else if (k == L"uLogLevel") g_cfg.LogLevel = _wtoi(v.c_str());
        else if (k == L"sLogDir") g_cfg.LogDir = v;
        else return false;
        return true;
    }

    void LoadConfig()
    {
        g_cfg = Config{};
        for (const auto& raw : ReadLines(g_mod / "NorseHUD.ini"))
        {
            std::wstring l = Trim(StripComment(raw));
            size_t eq = l.find(L'=');
            if (l.empty() || l[0] == L'[' || eq == std::wstring::npos) continue;
            ApplyKey(Trim(l.substr(0, eq)), Trim(l.substr(eq + 1)));
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
    // the ability strip (2026-11-20): NorseRealtimeNative's five LT controls - NrnAbilitySlot is NRN's export layout (24 bytes)
    struct NrnAbilitySlot
    {
        void* ability;
        float cooldownLeft, cooldownTotal, staminaCost;
        int32_t flags; // 1 an ability is on the control, 2 enough stamina, 4 off cooldown
    };
    static_assert(sizeof(NrnAbilitySlot) == 24, "NorseRealtimeNative's NrnAbilitySlot layout");
    using NrnSlotsFn = int (*)(void*, int, int);
    using NrnIntFn = int (*)();
    NrnSlotsFn g_abilitySlots = nullptr;
    NrnIntFn g_ltHeld = nullptr;

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
        if (!g_abilitySlots)
        {
            g_abilitySlots = Export<NrnSlotsFn>(L"NorseRealtimeNative", "NRN_GetAbilitySlots");
            g_ltHeld = Export<NrnIntFn>(L"NorseRealtimeNative", "NRN_LtHeld");
            static bool told = false;
            if (!told && g_hero) // NorseRealtimeNative is loaded: say once whether it has the strip's export
            {
                told = true;
                if (g_abilitySlots) NRN_INFO("STRIP: NorseRealtimeNative's NRN_GetAbilitySlots found - the ability strip shows in real-time fights");
                else NRN_WARN("STRIP: NorseRealtimeNative has no NRN_GetAbilitySlots (older than 2026-11-20) - no ability strip");
            }
        }
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
    UObject* g_teamLib = nullptr; // lives-forever (the team function library's Default__ object)
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
        int noTeam = 0, hidden = 0;
        for (UObject* a : actors)
        {
            if (!Alive(a, IndexOf(a))) continue; // destroyed, waiting for the garbage collector
            // not in the world yet: a mission's enemies wait as hidden units with no team until their fight spawns them
            // (SeasideSkirmish 2026-11-22: 24 of them drew red bars in empty forest). No bar for either.
            if (FieldTrue(a, L"bHidden"))
            {
                ++hidden;
                continue;
            }
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
            bool teamRead = g_teamLib && GetTeamIDSafe(g_teamLib, a, team);
            if (teamRead && team == 255) // no team: not in any fight yet (the combat mod counts these out too)
            {
                ++noTeam;
                continue;
            }
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
        static std::string lastUnits;
        std::string now = std::to_string(actors.size()) + " unit(s): " + std::to_string(out.size()) + " in the world, " + std::to_string(hidden) +
                          " hidden and " + std::to_string(noTeam) + " with no team skipped (no bar until they are spawned)";
        if (now != lastUnits) // logged when the counts change, not every second
        {
            lastUnits = now;
            NRN_INFO("units: %s", now.c_str());
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

    // ------------------------------------------------------------------ the ability strip (2026-11-20)
    // Controller-menus build list item 4: in a real-time fight (NorseRealtimeNative possesses the hero) a row of five boxes
    // at the bottom centre - LT + LB (war cry), LT + X / Y / A / B - each with the ability's own icon and name from its data
    // asset (UInfoAsset Icon / Name), a dark shutter falling as the cooldown runs out with the seconds left, red when there
    // is not enough stamina, a gold frame while LT is held. The game's turn-based ability bar (WBP_AbilityBar / WBP_APBar)
    // is collapsed while the strip shows and given back its visibility when the fight ends.
    struct StripInfo
    {
        UObject* icon = nullptr; // alive-checked via iconIx before each draw
        int32_t iconIx = -1;
        std::wstring name;
    };
    std::map<void*, StripInfo> g_stripInfo; // key-only: ability data asset -> its icon and name (cleared at each map load)
    bool g_stripShown = false, g_texOk = true;
    std::string g_stripLast; // the state last logged
    double g_stripNextLog = 0, g_barNextCheck = 0;
    struct HiddenBar
    {
        UObject* w; // alive-checked via ix before the visibility is given back
        int32_t ix;
        uint8_t vis;
    };
    std::vector<HiddenBar> g_hiddenBars;

    const StripInfo& InfoOf(void* ab)
    {
        auto it = g_stripInfo.find(ab);
        if (it != g_stripInfo.end()) return it->second;
        StripInfo s;
        UObject* a = static_cast<UObject*>(ab);
        if (auto** ic = Reflect::Get().Prop<UObject*>(a, L"Icon"); ic && *ic)
        {
            s.icon = *ic;
            s.iconIx = IndexOf(*ic);
        }
        auto* cls = reinterpret_cast<UStruct*>(Reflect::Get().ClassOf(a));
        FProperty* p = cls ? FindProp(cls, L"Name") : nullptr;
        std::wstring t = p ? norse::Export(p, reinterpret_cast<uint8_t*>(a) + p->GetOffset_Internal(), a) : L"";
        size_t b = t.rfind(L'"'), q = b == std::wstring::npos ? b : t.rfind(L'"', b - 1);
        if (q != std::wstring::npos && b > q) s.name = t.substr(q + 1, b - q - 1);
        if (s.name.empty()) s.name = ShortName(a->GetFullName(nullptr));
        NRN_INFO("STRIP: %s - name '%s', icon %s", Narrow(ShortName(a->GetFullName(nullptr))).c_str(), Narrow(s.name).c_str(),
                 s.icon ? Narrow(ShortName(s.icon->GetFullName(nullptr))).c_str() : "none");
        return g_stripInfo.emplace(ab, s).first->second;
    }

    // AHUD::DrawTexture(Texture @0, ScreenX @8, Y @12, W @16, H @20, U @24, V @28, UWidth @32, VHeight @36, FLinearColor Tint @40,
    // EBlendMode @56, Scale @60, bScalePosition @64, Rotation @68, FVector2D RotPivot @72) = 88 bytes
    void Tex(UObject* hud, UObject* tex, float x, float y, float w, float h, const Color& tint)
    {
        if (!g_texOk || !tex) return;
        Buf p;
        std::memcpy(p.b, &tex, 8);
        float v[8] = {x, y, w, h, 0, 0, 1, 1};
        std::memcpy(p.b + 8, v, 32);
        std::memcpy(p.b + 40, &tint, 16);
        p.b[56] = 2; // BLEND_Translucent
        float one = 1;
        std::memcpy(p.b + 60, &one, 4);
        if (!Reflect::Get().Call(hud, L"DrawTexture", p.b, 88, sizeof(p.b)))
        {
            g_texOk = false;
            NRN_WARN("STRIP: AHUD::DrawTexture refused (parameters %u bytes, 88 expected) - the strip shows names without icons",
                     Reflect::Get().ParmsSizeOf(hud, L"DrawTexture"));
        }
    }

    uint8_t Visibility(UObject* w)
    {
        Buf p;
        return Reflect::Get().Call(w, L"GetVisibility", p.b, 1, sizeof(p.b)) ? p.b[0] : 255;
    }

    void SetVisibility(UObject* w, uint8_t v)
    {
        Buf p;
        p.b[0] = v;
        Reflect::Get().Call(w, L"SetVisibility", p.b, 1, sizeof(p.b));
    }

    // the game's turn-based ability bar and AP bar: collapsed while the strip shows (checked every 2 s - a fight's HUD may
    // build them later), each one's own visibility given back when the strip goes
    void TurnBarsOff(double now)
    {
        if (now < g_barNextCheck) return;
        g_barNextCheck = now + 2.0;
        for (const wchar_t* cls : {L"WBP_AbilityBar_C", L"WBP_APBar_C"})
        {
            std::vector<UObject*> ws;
            try
            {
                RC::Unreal::UObjectGlobals::FindAllOf(cls, ws);
            }
            catch (...)
            {
            }
            int seen = 0, hidden = 0;
            for (UObject* w : ws)
            {
                if (!Alive(w, IndexOf(w)) || Name(w).find("Default__") != std::string::npos) continue;
                ++seen;
                uint8_t v = Visibility(w);
                if (v == 255 || v == 1 || v == 2) continue; // unreadable, or already collapsed / hidden
                g_hiddenBars.push_back({w, IndexOf(w), v});
                SetVisibility(w, 1);
                ++hidden;
                NRN_INFO("STRIP: the game's turn-based %s %s collapsed (was visibility %u; now %u)", Narrow(cls).c_str(), Name(w).c_str(), v,
                         Visibility(w));
            }
            static std::map<std::wstring, int> lastSeen;
            if (lastSeen[cls] != seen)
            {
                lastSeen[cls] = seen;
                NRN_INFO("STRIP: %d %s widget(s) in the world, %d visible ones collapsed now", seen, Narrow(cls).c_str(), hidden);
            }
        }
    }

    void TurnBarsBack()
    {
        int n = 0;
        for (const HiddenBar& h : g_hiddenBars)
            if (Alive(h.w, h.ix))
            {
                SetVisibility(h.w, h.vis);
                ++n;
            }
        if (!g_hiddenBars.empty()) NRN_INFO("STRIP: %d turn-based bar widget(s) given back their visibility", n);
        g_hiddenBars.clear();
    }

    void DrawStrip(UObject* hud, int32_t sx, int32_t sy, double now)
    {
        NrnAbilitySlot s[5]{};
        int n = g_cfg.AbilityStrip && g_abilitySlots && sx > 0 ? g_abilitySlots(s, 5, static_cast<int>(sizeof(NrnAbilitySlot))) : 0;
        if (n <= 0)
        {
            if (g_stripShown)
            {
                g_stripShown = false;
                g_stripLast.clear();
                NRN_INFO("STRIP: hidden (no real-time fight)");
                TurnBarsBack();
            }
            return;
        }
        static const wchar_t* kBtn[5] = {L"LB", L"X", L"Y", L"A", L"B"};
        static const Color kBtnCol[5] = {{0.8f, 0.8f, 0.8f, 1}, {0.25f, 0.55f, 1, 1}, {1, 0.85f, 0.2f, 1}, {0.3f, 0.85f, 0.3f, 1}, {0.95f, 0.3f, 0.25f, 1}};
        bool lt = g_ltHeld && g_ltHeld() != 0;
        float S = g_cfg.StripSize, gap = S * 0.25f, total = n * S + (n - 1) * gap;
        float x0 = sx / 2.f - total / 2, y = sy - g_cfg.StripBottom - S;
        std::string state = lt ? "[LT held] " : "";
        for (int i = 0; i < n; ++i)
        {
            float x = x0 + i * (S + gap);
            const NrnAbilitySlot& a = s[i];
            bool has = (a.flags & 1) != 0, stam = (a.flags & 2) != 0, ready = (a.flags & 4) != 0;
            const Color fc = lt ? Color{0.95f, 0.75f, 0.2f, 0.95f} : Color{0, 0, 0, 0.7f}; // the frame: four edges, gold while LT is held
            float t = lt ? 3.f : 2.f;
            Rect(hud, fc, x - t, y - t, S + 2 * t, t);
            Rect(hud, fc, x - t, y + S, S + 2 * t, t);
            Rect(hud, fc, x - t, y, t, S);
            Rect(hud, fc, x + S, y, t, S);
            Rect(hud, {0.05f, 0.05f, 0.05f, has ? 0.75f : 0.35f}, x, y, S, S);
            Text(hud, std::wstring(L"LT+") + kBtn[i], kBtnCol[i], x + 3, y - 20, 1.0f);
            if (!has)
            {
                state += Narrow(kBtn[i]) + " -; ";
                continue;
            }
            const StripInfo& info = InfoOf(a.ability);
            if (info.icon && Alive(info.icon, info.iconIx))
                Tex(hud, info.icon, x + 4, y + 4, S - 8, S - 8, ready && stam ? Color{1, 1, 1, 1} : Color{0.45f, 0.45f, 0.45f, 1});
            if (!ready && a.cooldownTotal > 0) // the shutter: the part still cooling, from the top
            {
                float frac = std::clamp(a.cooldownLeft / a.cooldownTotal, 0.f, 1.f);
                Rect(hud, {0, 0, 0, 0.65f}, x, y, S, S * frac);
                wchar_t sec[16];
                swprintf(sec, 16, L"%.0f", std::ceil(a.cooldownLeft));
                Text(hud, sec, {1, 1, 1, 1}, x + S / 2 - 8 * std::wcslen(sec) / 2.f, y + S / 2 - 10, 1.3f);
            }
            if (!stam) Rect(hud, {0.8f, 0.1f, 0.05f, 0.35f}, x, y, S, S); // not enough stamina now
            std::wstring nm = info.name.size() > 12 ? info.name.substr(0, 11) + L"." : info.name;
            Text(hud, nm, {0.95f, 0.9f, 0.8f, has ? 1.f : 0.5f}, x + S / 2 - 3.5f * nm.size(), y + S + 4, 0.9f);
            char buf[96];
            snprintf(buf, sizeof(buf), "%s %s %s; ", Narrow(kBtn[i]).c_str(), Narrow(info.name).c_str(),
                     !ready ? (std::to_string(static_cast<int>(std::ceil(a.cooldownLeft))) + " s").c_str() : stam ? "ready" : "no stamina");
            state += buf;
        }
        if (!g_stripShown)
        {
            g_stripShown = true;
            NRN_INFO("STRIP: shown at (%.0f, %.0f), %d slots of %.0f px, canvas %dx%d", x0, y, n, S, sx, sy);
        }
        TurnBarsOff(now);
        // log each change of state; while a cooldown runs this is once a second (the countdown, for the test)
        if (state != g_stripLast && now >= g_stripNextLog)
        {
            g_stripLast = state;
            g_stripNextLog = now + 0.5;
            NRN_INFO("STRIP: %s", state.c_str());
        }
    }

    uint64_t g_frames = 0, g_drawn = 0;

    // ------------------------------------------------------------------ the compass (2026-11-22)
    // The owner, 2026-11-22 05:21: "we need an in-game compass that identifies the location of quest objectives. Just like
    // Skyrim's compass." A strip at the top centre showing 180 degrees around the camera's heading: N / E / S / W and a tick
    // every 15 degrees, and a gold marker at the bearing of the current quest objective while it is within that view.
    // The objective: the mission's AMissionObjectives singleton keeps the current destination as DestinationWaypoint (a
    // world position) and DestinationMarker (an actor); the game's own WBP_Waypoint arrow belongs to the top-down grid
    // camera and its stagnation timer, so in real-time play nothing showed where to go. North is world +X (yaw 0), east +Y.
    UObject* g_objectives = nullptr; // alive-checked via g_objectivesIx before every use
    int32_t g_objectivesIx = -1;
    bool g_haveObjective = false;
    FVec g_objectiveAt{};

    // KismetSystemLibrary::Conv_SoftObjectReferenceToObject(TSoftObjectPtr<UObject> @0 (40 bytes)) -> UObject* @40
    UObject* FromSoft(const uint8_t* soft)
    {
        static UObject* ksl = nullptr; // lives-forever (Default__KismetSystemLibrary)
        if (!ksl) ksl = RC::Unreal::UObjectGlobals::StaticFindObject_InternalSlow(nullptr, nullptr, L"/Script/Engine.Default__KismetSystemLibrary", false);
        if (!ksl) return nullptr;
        static bool sizeOk = Reflect::Get().ParmsSizeOf(ksl, L"Conv_SoftObjectReferenceToObject") == 48;
        if (!sizeOk) return nullptr;
        Buf p;
        std::memcpy(p.b, soft, 40);
        if (!Reflect::Get().Call(ksl, L"Conv_SoftObjectReferenceToObject", p.b, 48, sizeof(p.b))) return nullptr;
        UObject* o = nullptr;
        std::memcpy(&o, p.b + 40, 8);
        return o && Alive(o, IndexOf(o)) ? o : nullptr;
    }

    // AMissionObjectives::Objectives - TMap<FName, FMissionObjective> - read by hand (the layout NorseReflect's Rows() reads,
    // with a 0x170-byte value): element = key FName @0, FMissionObjective @8 (ID @0, MissionObjectives* @8, Hidden @0x8A,
    // Destination soft @0x90, DestinationPriority @0xB8, DestinationMarker soft @0xC0, Active @0x118, Result @0x119), then
    // the hash ids - 0x180 bytes. Each element is checked (its ID equals its key and it points back at this actor) before
    // anything in it is used. Returns the destination (its marker if it has one) of the active, shown objective with the
    // highest DestinationPriority, and logs the objective list when it changes.
    UObject* ActiveObjectiveDestination(UObject* objectives)
    {
        auto* h = Reflect::Get().Prop<uint8_t>(objectives, L"Objectives");
        if (!h) return nullptr;
        struct SparseHead
        {
            uint8_t* data;
            int32_t num, max;
            uint32_t inl[4];
            uint32_t* secondary;
            int32_t numBits, maxBits;
            int32_t firstFree, numFree;
        };
        auto* s = reinterpret_cast<SparseHead*>(h);
        static std::string lastList;
        if (s->num < 0 || s->num > 512 || s->num > s->max || (s->num && !s->data) || s->numFree < 0 || s->numFree > s->num)
        {
            if (lastList != "bad") NRN_WARN("COMPASS: the Objectives map does not look like a TMap (num %d) - left alone", s->num);
            lastList = "bad";
            return nullptr;
        }
        const uint32_t* bits = s->secondary ? s->secondary : s->inl;
        UObject* best = nullptr;
        int bestPrio = INT_MIN;
        std::string list;
        for (int32_t i = 0; i < s->num; ++i)
        {
            if (s->numFree && !(bits[i >> 5] & (1u << (i & 31)))) continue;
            uint8_t* el = s->data + static_cast<size_t>(i) * 0x180;
            uint8_t* obj = el + 8;
            UObject* owner = nullptr;
            std::memcpy(&owner, obj + 8, 8);
            if (std::memcmp(el, obj, 8) != 0 || owner != objectives)
            {
                if (lastList != "layout") NRN_WARN("COMPASS: objective %d does not match the expected layout (ID vs key, owner) - map left alone", i);
                lastList = "layout";
                return nullptr;
            }
            uint64_t key = 0;
            std::memcpy(&key, el, 8);
            bool hidden = obj[0x8A] != 0, active = obj[0x118] != 0;
            uint8_t result = obj[0x119];
            int32_t prio = 0;
            std::memcpy(&prio, obj + 0xB8, 4);
            UObject* dest = active && !hidden ? FromSoft(obj + 0x90) : nullptr;
            UObject* mark = active && !hidden ? FromSoft(obj + 0xC0) : nullptr;
            UObject* use = mark ? mark : dest;
            list += " " + Narrow(NameText(key)) + "[" + (active ? "active" : "-") + (hidden ? ",hidden" : "") + ",result " + std::to_string(result) +
                    (use ? ", -> " + Name(use) : "") + "]";
            if (use && prio > bestPrio)
            {
                best = use;
                bestPrio = prio;
            }
        }
        if (list != lastList)
        {
            lastList = list;
            NRN_INFO("COMPASS: %d objective(s):%s", s->num - s->numFree, list.c_str());
        }
        return best;
    }

    // UObjectBase (UE5): vtable @0, flags @8, index @0xC, ClassPrivate @0x10, NamePrivate @0x18, OuterPrivate @0x20. The
    // ABI header declares no outer accessor, so the outer is read at 0x20 - only after the class pointer is checked at 0x10.
    UObject* OuterOf(UObject* o)
    {
        if (!o) return nullptr;
        auto* b = reinterpret_cast<uint8_t*>(o);
        UObject* cls = nullptr;
        std::memcpy(&cls, b + 0x10, 8);
        if (cls != reinterpret_cast<UObject*>(Reflect::Get().ClassOf(o))) return nullptr;
        UObject* outer = nullptr;
        std::memcpy(&outer, b + 0x20, 8);
        return outer && Alive(outer, IndexOf(outer)) ? outer : nullptr;
    }

    std::vector<FVec> g_talkTargets;

    // Every Activity_Dialogue in the map whose interactable (the first InteractableComponent in its outer chain) is not
    // Completed: the interactable's actor location. Returns the source text for the log, or "none".
    std::string DialogueTargets(std::vector<FVec>& out)
    {
        std::vector<UObject*> dlg;
        try
        {
            RC::Unreal::UObjectGlobals::FindAllOf(L"Activity_Dialogue", dlg);
        }
        catch (...)
        {
        }
        std::string names;
        for (UObject* d : dlg)
        {
            if (!d || !Alive(d, IndexOf(d)) || d->GetFullName(nullptr).find(L"Default__") != std::wstring::npos) continue;
            UObject* comp = nullptr;
            for (UObject* o = OuterOf(d); o; o = OuterOf(o))
            {
                UObject* cls = reinterpret_cast<UObject*>(Reflect::Get().ClassOf(o));
                if (cls && Name(cls).find("InteractableComponent") != std::string::npos)
                {
                    comp = o;
                    break;
                }
            }
            if (!comp || FieldTrue(comp, L"Completed")) continue;
            UObject* actor = OuterOf(comp);
            FVec at{};
            if (!actor || !GetVec(actor, L"K2_GetActorLocation", at) || (at.X == 0 && at.Y == 0 && at.Z == 0)) continue;
            out.push_back(at);
            names += (names.empty() ? "" : ", ") + Name(actor);
        }
        return out.empty() ? "none" : "dialogue waiting at " + names;
    }

    void RefreshObjective()
    {
        if (!g_cfg.Compass) return;
        if (!g_objectives || !Alive(g_objectives, g_objectivesIx))
        {
            g_objectives = nullptr;
            std::vector<UObject*> found;
            try
            {
                RC::Unreal::UObjectGlobals::FindAllOf(L"MissionObjectives", found);
            }
            catch (...)
            {
            }
            for (UObject* o : found)
                if (Alive(o, IndexOf(o)) && o->GetFullName(nullptr).find(L"Default__") == std::wstring::npos)
                {
                    g_objectives = o;
                    g_objectivesIx = IndexOf(o);
                    break;
                }
        }
        std::string source = "none";
        FVec at{};
        if (g_objectives)
        {
            auto* wp = Reflect::Get().Prop<FVec>(g_objectives, L"DestinationWaypoint");
            auto** marker = Reflect::Get().Prop<UObject*>(g_objectives, L"DestinationMarker");
            if (wp && (wp->X != 0 || wp->Y != 0 || wp->Z != 0))
            {
                at = *wp;
                source = "DestinationWaypoint";
            }
            else if (marker && *marker && Alive(*marker, IndexOf(*marker)) && GetVec(*marker, L"K2_GetActorLocation", at))
                source = "DestinationMarker " + Name(*marker);
            else if (UObject* dest = ActiveObjectiveDestination(g_objectives)) // first test: "Speak with the Homestead Owner" set neither
            {
                if (GetVec(dest, L"K2_GetActorLocation", at)) source = "the active objective's Destination " + Name(dest);
            }
        }
        // Second test (SeasideSkirmish, probes objective_setters / activity_owners): "Speak with the Village Chief" has no
        // Destination and no condition - an interactable (InteractableVolume_1) holds the chief's Activity_Dialogue and the
        // StartCombat that completes it. So, when the mission names no place: every interactable still waiting with a
        // dialogue is a target, as Skyrim marks the person to talk to; using it (Completed) takes its marker away.
        g_talkTargets.clear();
        if (g_objectives && source == "none") source = DialogueTargets(g_talkTargets);
        else if (source != "none") g_talkTargets.push_back(at); // one marker list, whatever the source
        g_haveObjective = !g_talkTargets.empty();
        if (g_haveObjective) at = g_talkTargets.front();
        static std::string last;
        std::string now = !g_objectives ? "no mission objectives in this map"
                          : g_haveObjective ? std::to_string(g_talkTargets.size()) + " marker(s), first at (" + std::to_string(static_cast<int>(at.X)) + ", " +
                                                  std::to_string(static_cast<int>(at.Y)) + ", " + std::to_string(static_cast<int>(at.Z)) + ") from " + source
                                            : "mission objectives found (" + Name(g_objectives) + "), no destination set (waypoint zero, no marker)";
        if (now != last)
        {
            last = now;
            NRN_INFO("COMPASS: %s", now.c_str());
        }
    }

    double Wrap180(double a)
    {
        a = std::fmod(a + 180.0, 360.0);
        if (a < 0) a += 360.0;
        return a - 180.0;
    }

    void DrawCompass(UObject* hud, int32_t sx)
    {
        if (!g_cfg.Compass || !g_objectives || !Alive(g_objectives, g_objectivesIx) || sx <= 0) return;
        auto** pc = Reflect::Get().Prop<UObject*>(hud, L"PlayerOwner");
        auto** cam = pc && *pc ? Reflect::Get().Prop<UObject*>(*pc, L"PlayerCameraManager") : nullptr;
        FRot r{};
        if (!cam || !*cam || !GetRot(*cam, L"GetCameraRotation", r)) return;
        const float W = g_cfg.CompassWidth, H = 26, top = g_cfg.CompassTop, cx = sx / 2.f, perDeg = W / 180.f;
        Rect(hud, {0, 0, 0, 0.45f}, cx - W / 2, top, W, H);
        Rect(hud, {0.85f, 0.8f, 0.7f, 0.5f}, cx - W / 2, top + H, W, 1);
        static const wchar_t* kLetters[] = {L"N", L"E", L"S", L"W"};
        for (int a = 0; a < 360; a += 15)
        {
            double rel = Wrap180(a - r.Yaw);
            if (std::fabs(rel) > 90) continue;
            float x = cx + static_cast<float>(rel) * perDeg;
            float fade = 1.f - static_cast<float>(std::fabs(rel)) / 110.f; // dimmer toward the ends, as Skyrim's
            if (a % 90 == 0)
                Text(hud, kLetters[a / 90], {0.95f, 0.92f, 0.85f, fade}, x - 5, top + 4, 1.1f);
            else
                Rect(hud, {0.85f, 0.8f, 0.7f, 0.7f * fade}, x - 0.5f, top + (a % 45 == 0 ? 8.f : 12.f), 1, a % 45 == 0 ? 12.f : 6.f);
        }
        if (g_haveObjective)
        {
            FVec from{};
            if (GetVec(*cam, L"GetCameraLocation", from))
                for (const FVec& t : g_talkTargets)
                {
                    double bearing = std::atan2(t.Y - from.Y, t.X - from.X) * 180.0 / 3.14159265358979;
                    double rel = Wrap180(bearing - r.Yaw);
                    if (std::fabs(rel) > 90) continue;
                    // the owner, 05:47: "the compass still isn't showing me where I'm supposed to go" - the first 10 px
                    // marker was drawn but too small to notice: a larger gold marker overhanging the strip, and the distance
                    float x = cx + static_cast<float>(rel) * perDeg;
                    Rect(hud, {0, 0, 0, 0.85f}, x - 10, top - 4, 20, H + 8);
                    Rect(hud, {1.0f, 0.78f, 0.18f, 1}, x - 8, top - 2, 16, H + 4);
                    Rect(hud, {0.25f, 0.15f, 0.02f, 1}, x - 3, top + 5, 6, H - 10);
                    double dx = t.X - from.X, dy = t.Y - from.Y;
                    int metres = static_cast<int>(std::sqrt(dx * dx + dy * dy) / 100.0 + 0.5);
                    std::wstring d = std::to_wstring(metres) + L" m";
                    Text(hud, d, {1.0f, 0.85f, 0.4f, 1}, x - 4.f * static_cast<float>(d.size()), top + H + 6, 1.0f);
                }
        }
    }

    // 2026-11-22: other mods draw on the same canvas through NHUD_AddDrawCallback (NorseQuickLoot's loot panel); each is
    // called after NorseHUD's own drawing, on the game thread, with the HUD and the canvas size, and calls the HUD's own
    // DrawRect / DrawText / DrawTexture itself. One hook on DrawHUD for every mod - a second hook on the same function
    // from another DLL is what the address library refuses (past-mistakes 15279).
    using DrawCb = void (*)(void* hud, int sizeX, int sizeY);
    std::vector<DrawCb> g_drawCbs;

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
            RefreshObjective();
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
            float w = sx * 0.4f, down = g_cfg.Compass && g_objectives ? g_cfg.CompassTop + 40.f : 0.f; // below the compass when it shows
            Bar(hud, sx / 2.f, 40.f + down, w, 12.f, static_cast<float>(Health(*boss)) / std::max(1, boss->maxHp), {0.7f, 0.05f, 0.05f, 0.95f});
            Text(hud, Widen(boss->name), {0.95f, 0.9f, 0.8f, 1}, sx / 2.f - w / 2, 18.f + down, 1.2f);
        }
        DrawStrip(hud, sx, sy, now);
        DrawCompass(hud, sx);
        for (DrawCb cb : g_drawCbs) cb(hud, sx, sy); // each guards itself (NorseQuickLoot: try/catch around its panel)
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

    // ------------------------------------------------------------------ console: nhud.status | nhud.set <key> <value> | nhud.reload
    void ResetCondition() // sBarCondition changed: compile it again on the next refresh
    {
        g_barCond = 0;
        g_ncEval = nullptr;
        g_nextRefresh = 0;
    }

    void Status()
    {
        int shown = 0, bosses = 0, enemies = 0;
        for (const Unit& u : g_units)
        {
            shown += u.show;
            bosses += u.boss;
            enemies += u.enemy;
        }
        NRN_INFO("nhud.status: v%s, drawing %s, DrawHUD slot %zu, %zu HUD class(es) hooked", NH_VERSION,
                 g_hudOff ? "OFF (see the errors above)" : "on", g_drawSlot, g_drawOriginals.size());
        NRN_INFO("  bBars %d, bBossBar %d, bLockMarker %d, fBarWidth %.0f, fBarHeight %.0f, fBarZ %.0f, fMaxDistance %.0f, uLogLevel %d",
                 g_cfg.Bars, g_cfg.BossBar, g_cfg.LockMarker, g_cfg.BarWidth, g_cfg.BarHeight, g_cfg.BarZ, g_cfg.MaxDistance, g_cfg.LogLevel);
        NRN_INFO("  sBarCondition '%s': %s", Narrow(g_cfg.BarCondition).c_str(),
                 g_barCond ? "compiled (NorseConditions)" : g_ncEval ? "did not compile - every living unit shown" : "NorseConditions not found yet - every living unit shown");
        NRN_INFO("  ability strip: bAbilityStrip %d, fStripSize %.0f, fStripBottom %.0f, NRN_GetAbilitySlots %s, %s; %zu turn-based bar widget(s) collapsed",
                 g_cfg.AbilityStrip, g_cfg.StripSize, g_cfg.StripBottom, g_abilitySlots ? "found" : "not found", g_stripShown ? "shown" : "not shown",
                 g_hiddenBars.size());
        NRN_INFO("  lock-on target %s, hero %s (NorseRealtimeNative)", g_lockTarget ? "export found" : "export not found", g_hero ? "export found" : "export not found");
        NRN_INFO("  units: %zu known, %d with a bar shown, %d enemy, %d boss; frames %llu, bars drawn %llu%s", g_units.size(), shown, enemies, bosses,
                 static_cast<unsigned long long>(g_frames), static_cast<unsigned long long>(g_drawn), g_drawOk ? "" : " - DrawRect calls FAILED");
    }

    bool RunCommand(const std::wstring& c)
    {
        std::wstring cmd = Trim(c), arg;
        if (size_t sp = cmd.find(L' '); sp != std::wstring::npos)
        {
            arg = Trim(cmd.substr(sp + 1));
            cmd = cmd.substr(0, sp);
        }
        cmd = Lower(cmd);
        NRN_INFO("command: %s %s", Narrow(cmd).c_str(), Narrow(arg).c_str());
        if (cmd == L"nhud.status") Status();
        else if (cmd == L"nhud.reload")
        {
            LoadConfig();
            Log::Get().SetLevel(g_cfg.LogLevel);
            ResetCondition();
            NRN_INFO("nhud.reload: NorseHUD.ini read again (sLogDir takes effect at the next start)");
            Status();
        }
        else if (cmd == L"nhud.set" && !arg.empty())
        {
            size_t sp = arg.find(L' ');
            std::wstring key = Trim(arg.substr(0, sp)), value = sp == std::wstring::npos ? L"" : Trim(arg.substr(sp + 1));
            std::wstring match;
            for (const wchar_t* k : {L"bBars", L"bBossBar", L"bLockMarker", L"sBarCondition", L"fBarWidth", L"fBarHeight", L"fBarZ", L"fMaxDistance", L"bAbilityStrip",
                                       L"fStripSize", L"fStripBottom", L"bCompass", L"fCompassWidth", L"fCompassTop", L"uLogLevel"})
                if (_wcsicmp(k, key.c_str()) == 0) match = k;
            if (match.empty() || value.empty())
                NRN_WARN("nhud.set: use nhud.set <key> <value>, key one of bBars bBossBar bLockMarker sBarCondition fBarWidth fBarHeight fBarZ fMaxDistance bAbilityStrip fStripSize fStripBottom bCompass fCompassWidth fCompassTop uLogLevel");
            else
            {
                ApplyKey(match, value);
                if (match == L"uLogLevel") Log::Get().SetLevel(g_cfg.LogLevel);
                if (match == L"sBarCondition") ResetCondition();
                NRN_INFO("nhud.set %s = %s (this session only; NorseHUD.ini is not changed)", Narrow(match).c_str(), Narrow(value).c_str());
            }
        }
        else NRN_WARN("unknown command - nhud.status | nhud.set <key> <value> | nhud.reload");
        return true;
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
        Log::Get().Open((g_cfg.LogDir.empty() ? g_mod : fs::path(g_cfg.LogDir)) / "NorseHUD.log", g_cfg.LogLevel);
        Log::Get().Write(Level::Error, "NorseHUD v%s - log level %d - bars %d, boss bar %d, lock marker %d", NH_VERSION, g_cfg.LogLevel, g_cfg.Bars, g_cfg.BossBar, g_cfg.LockMarker);
    }

    auto on_unreal_init() -> void override
    {
        try
        {
            RC::Unreal::Hook::RegisterInitGameStatePostCallback([](RC::Unreal::AGameModeBase*) {
                g_units.clear();
                g_nextRefresh = 0;
                g_stripInfo.clear(); // a new map: ability assets and icons may be other objects
                g_hiddenBars.clear();
                g_stripShown = false;
                g_stripLast.clear();
                HookHud();
            });
            RC::Unreal::Hook::RegisterEngineTickPreCallback([](RC::Unreal::UEngine*, float) {
                static int n = 0;
                if (++n % 120 == 0) HookHud(); // a HUD class first seen mid-map (cheap: one FindFirstOf every ~2 s)
            });
            RC::Unreal::Hook::RegisterProcessConsoleExecCallback(
                    [](UObject*, const wchar_t* cmd, RC::Unreal::FOutputDevice&, UObject*) -> bool {
                        if (!cmd || _wcsnicmp(cmd, L"nhud.", 5) != 0) return false;
                        static ULONGLONG last = 0;
                        static std::wstring lastCmd;
                        ULONGLONG now = GetTickCount64();
                        if (lastCmd == cmd && now - last < 250) return true; // the console delivers a line more than once
                        last = now;
                        lastCmd = cmd;
                        return RunCommand(cmd);
                    });
            NRN_INFO("callbacks registered");
        }
        catch (const std::exception& e)
        {
            NRN_ERROR("callback registration failed: %s", e.what());
        }
    }
};

// 2026-11-22: draw on NorseHUD's canvas - fn(hud, canvas width, canvas height) runs every HUD frame after NorseHUD's own
// drawing. Returns 1 when added (or already there), 0 for a null function. Call it on the game thread.
extern "C" __declspec(dllexport) int NHUD_AddDrawCallback(void (*fn)(void* hud, int sizeX, int sizeY))
{
    if (!fn) return 0;
    if (std::find(g_drawCbs.begin(), g_drawCbs.end(), fn) == g_drawCbs.end())
    {
        g_drawCbs.push_back(fn);
        NRN_INFO("draw callback added (%zu in all) - another mod draws on the HUD canvas", g_drawCbs.size());
    }
    return 1;
}

extern "C" __declspec(dllexport) RC::CppUserModBase* start_mod()
{
    return new NorseHUD();
}

extern "C" __declspec(dllexport) void uninstall_mod(RC::CppUserModBase* mod)
{
    delete mod;
}

// =========================
// File: LOAPlugin.cpp
// =========================

#include "stdafx.h"
#include "LOAPlugin.h"
#include <windows.h>
#include <fstream>
#include <shlwapi.h>
#include <unordered_set>
#include <json.hpp>

using json = nlohmann::json;

extern "C" IMAGE_DOS_HEADER __ImageBase;

// =============================
// Hashing Utilities
// =============================

size_t HashVectorOfStrings(const std::vector<std::string>& vec)
{
    size_t seed = vec.size();
    for (const auto& s : vec) {
        for (char c : s) {
            seed ^= c + 0x9e3779b9 + (seed << 6) + (seed >> 2);
        }
    }
    return seed;
}

size_t HashSetOfStrings(const std::unordered_set<std::string>& set)
{
    size_t seed = set.size();
    for (const auto& s : set) {
        for (char c : s) {
            seed ^= c + 0x9e3779b9 + (seed << 6) + (seed >> 2);
        }
    }
    return seed;
}

std::vector<LOAEntry> destinationLoas;
std::vector<LOAEntry> departureLoas;
std::vector<LOAEntry> lorArrivals;
std::vector<LOAEntry> lorDepartures;
std::vector<LOAEntry> fallbackLoas;

int nextFunctionId = 1000;

LOAPlugin::LOAPlugin()
    : CPlugIn(COMPATIBILITY_CODE, "LOA Plugin", "1.1", "Author", "LOA Plugin")
{
    static bool registered = false;
    if (!registered) {
        RegisterTagItemType("LOA XFL", 1996);
        RegisterTagItemType("LOA XFL Detailed", 2000);
        RegisterTagItemType("COP", 1997);
        registered = true;
    }

    std::string sector = ControllerMyself().GetPositionId();
    if (!sector.empty()) {
        LoadLOAsFromJSON();
    }
}

void LOAPlugin::OnControllerPositionUpdate(EuroScopePlugIn::CController controller)
{
    // Example: reload LOAs when controller position changes
    std::string sector = controller.GetPositionId();
    if (!sector.empty() && sector != this->loadedSector) {
        LoadLOAsFromJSON();
    }
}

LOAPlugin::~LOAPlugin() {}

void LOAPlugin::LoadLOAsFromJSON() {
    std::string mySector = ControllerMyself().GetPositionId();
    if (mySector.empty() || mySector == this->loadedSector) return;
    this->loadedSector = mySector;

    char dllPath[MAX_PATH];
    GetModuleFileNameA(HINSTANCE(&__ImageBase), dllPath, sizeof(dllPath));

    std::string basePath(dllPath);
    size_t lastSlash = basePath.find_last_of("\\/");
    basePath = (lastSlash != std::string::npos) ? basePath.substr(0, lastSlash) : ".";
    std::string filePath = basePath + "\\loa_configs_json\\" + mySector + ".json";

    std::ifstream inFile(filePath);
    if (!inFile.is_open()) {
        DisplayUserMessage("LOA Plugin", "JSON Load Error", ("Cannot open: " + filePath).c_str(), true, true, true, true, false);
        return;
    }

    json config;
    try {
        inFile >> config;
    }
    catch (const std::exception& e) {
        DisplayUserMessage("LOA Plugin", "JSON Parse Error", e.what(), true, true, true, true, false);
        return;
    }

    destinationLoas.clear();
    departureLoas.clear();
    lorArrivals.clear();
    lorDepartures.clear();
    fallbackLoas.clear();

    auto processAirportList = [](const std::vector<std::string>& list,
        std::unordered_set<std::string>& exact,
        std::vector<std::string>& prefixes)
        {
            for (const std::string& a : list) {
                if (a.length() == 4) exact.insert(a);
                else prefixes.push_back(a);
            }
        };

    auto parseLOAList = [&](const json& array, bool isFallback = false) {
        std::vector<LOAEntry> result;
        for (const auto& item : array) {
            LOAEntry loa;
            if (item.contains("origins")) {
                loa.originAirports = item["origins"].get<std::vector<std::string>>();
                processAirportList(loa.originAirports, loa.originAirportSet, loa.originAirportPrefixes);
            }
            if (item.contains("destinations")) {
                loa.destinationAirports = item["destinations"].get<std::vector<std::string>>();
                processAirportList(loa.destinationAirports, loa.destinationAirportSet, loa.destinationAirportPrefixes);
            }
            if (item.contains("waypoints")) loa.waypoints = item["waypoints"].get<std::vector<std::string>>();
            if (item.contains("nextSectors")) loa.nextSectors = item["nextSectors"].get<std::vector<std::string>>();
            if (item.contains("copText")) loa.copText = item["copText"].get<std::string>();
            if (item.contains("requireNextSectorOnline")) loa.requireNextSectorOnline = item["requireNextSectorOnline"].get<bool>();
            if (item.contains("xfl")) loa.xfl = item["xfl"].get<int>();
            if (item.contains("minAltitudeFt")) loa.minAltitudeFt = item["minAltitudeFt"].get<int>();
            result.push_back(loa);
        }
        return result;
        };

    if (config.contains("destinationLoas")) destinationLoas = parseLOAList(config["destinationLoas"]);
    if (config.contains("departureLoas")) departureLoas = parseLOAList(config["departureLoas"]);
    if (config.contains("lorArrivals")) lorArrivals = parseLOAList(config["lorArrivals"]);
    if (config.contains("lorDepartures")) lorDepartures = parseLOAList(config["lorDepartures"]);
    if (config.contains("fallbackLoas")) fallbackLoas = parseLOAList(config["fallbackLoas"], true);

    DisplayUserMessage("LOA Plugin", "LOA Load Success", ("LOAs loaded for sector: " + mySector).c_str(), true, true, true, true, false);
}

bool LOAPlugin::IsLOARelevantState(int state) {
    static const std::unordered_set<int> validStates = {
        FLIGHT_PLAN_STATE_NOTIFIED,
        FLIGHT_PLAN_STATE_COORDINATED,
        FLIGHT_PLAN_STATE_TRANSFER_TO_ME_INITIATED,
        FLIGHT_PLAN_STATE_TRANSFER_FROM_ME_INITIATED,
        FLIGHT_PLAN_STATE_ASSUMED
    };
    return validStates.count(state) > 0;
}

const std::unordered_set<std::string>& LOAPlugin::GetOnlineControllersCached()
{
    ULONGLONG currentTime = GetTickCount64();
    if (currentTime - lastOnlineFetchTime > 5000 || cachedOnlineControllers.empty()) {
        cachedOnlineControllers.clear();
        for (EuroScopePlugIn::CController c = ControllerSelectFirst(); c.IsValid(); c = ControllerSelectNext(c)) {
            std::string callsign = c.GetCallsign();
            if (!callsign.empty() &&   // ✅ Only if callsign exists
                (callsign.find("_CTR") != std::string::npos ||
                    callsign.find("_APP") != std::string::npos)) {  // ✅ Only CTR/APP
                cachedOnlineControllers.insert(c.GetPositionId());
            }
        }
        lastOnlineFetchTime = currentTime;

        cachedOnlineControllersHash = HashSetOfStrings(cachedOnlineControllers);  // if you use controller hash caching
    }
    return cachedOnlineControllers;
}

bool LOAPlugin::IsControllerOnlineCached(const std::string& controllerId, const std::unordered_set<std::string>& onlineControllers)
{
    return onlineControllers.count(controllerId) > 0;
}

bool LOAPlugin::MatchesAirport(const std::unordered_set<std::string>& exactSet,
    const std::vector<std::string>& prefixes,
    const std::string& airport)
{
    // Fast exact match
    if (exactSet.count(airport) > 0) return true;

    // Prefix match
    for (const auto& prefix : prefixes) {
        if (airport.compare(0, prefix.length(), prefix) == 0)
            return true;
    }
    return false;
}

const std::vector<std::string>& LOAPlugin::GetCachedRoutePoints(const EuroScopePlugIn::CFlightPlan& fp) {
    static std::vector<std::string> empty;

    std::string callsign = fp.GetCallsign();
    ULONGLONG now = GetTickCount64(); // ✅ Safe and consistent

    // 3s cache validity
    if (routeCache.count(callsign) && now - routeCacheTime[callsign] < 3000) {
        return routeCache[callsign];
    }

    auto route = fp.GetExtractedRoute();
    std::vector<std::string> routePoints;
    for (int i = 0; i < route.GetPointsNumber(); ++i)
        routePoints.emplace_back(route.GetPointName(i));

    routeCache[callsign] = std::move(routePoints);
    routeCacheTime[callsign] = now;

    return routeCache[callsign];
}

void LOAPlugin::CleanupCache(const std::string& callsign) {
    matchedLOACache.erase(callsign);
    routeCache.erase(callsign);
    routeCacheTime.erase(callsign);
}

void LOAPlugin::OnFlightPlanStateChange(EuroScopePlugIn::CFlightPlan fp) {
    if (!fp.IsValid()) return;

    int state = fp.GetState();
    if (state == FLIGHT_PLAN_STATE_NON_CONCERNED || state == FLIGHT_PLAN_STATE_REDUNDANT) {
        CleanupCache(fp.GetCallsign());
    }
}

void LOAPlugin::OnFlightPlanCoordinationStateChange(CFlightPlan fp, int coordinationType, int newState)
{
    if (!fp.IsValid()) return;

    std::string callsign = fp.GetCallsign();

    // Only handle exit altitude coordination
    if (coordinationType == EuroScopePlugIn::TAG_ITEM_TYPE_COPN_COPX_ALTITUDE) {
        CoordinationInfo& info = coordinationStates[callsign];
        info.exitAltitude = fp.GetExitCoordinationAltitude();
        info.exitAltitudeState = newState;
    }

    // Handle point name coordination if needed
    if (coordinationType == EuroScopePlugIn::TAG_ITEM_TYPE_COPN_COPX_NAME) {
        CoordinationInfo& info = coordinationStates[callsign];
        info.exitPoint = fp.GetExitCoordinationPointName();
        info.exitPointState = newState;
    }
}

void LOAPlugin::OnGetTagItem(
    EuroScopePlugIn::CFlightPlan flightPlan,
    EuroScopePlugIn::CRadarTarget radarTarget,
    int itemCode,
    int tagData,
    char sItemString[16],
    int* pColorCode,
    COLORREF* pRGB,
    double* pFontSize)
{
    const std::string callsign = flightPlan.GetCallsign();
    const auto& fpd = flightPlan.GetFlightPlanData();
    int clearedAltitude = flightPlan.GetClearedAltitude();
    int finalAltitude = flightPlan.GetFinalAltitude();
    std::string origin = fpd.GetOrigin();
    std::string destination = fpd.GetDestination();

    plugin.lastTagData = { callsign, clearedAltitude, finalAltitude, origin, destination };

    // Precompute and cache route + controller data per frame
    ULONGLONG now = GetTickCount64();
    if (callsign != plugin.currentFrameCallsign || now - plugin.currentFrameTimestamp > 100) {
        plugin.currentFrameCallsign = callsign;
        plugin.currentFrameTimestamp = now;
        plugin.currentFrameOnlineControllers = plugin.GetOnlineControllersCached();
        plugin.currentFrameRoutePoints = plugin.GetCachedRoutePoints(flightPlan);
        plugin.currentFrameMatchedLOA = MatchLoaEntry(flightPlan, plugin.currentFrameOnlineControllers); // ✅ Add this

    }

    switch (itemCode)
    {
    case 1996:
        RenderXFLTagItem(flightPlan, radarTarget, tagData, sItemString, pColorCode, pRGB, pFontSize);
        break;
    case 2000:
        RenderXFLDetailedTagItem(flightPlan, radarTarget, tagData, sItemString, pColorCode, pRGB, pFontSize);
        break;
    case 1997:
        RenderCOPTagItem(flightPlan, radarTarget, tagData, sItemString, pColorCode, pRGB, pFontSize);
        break;
    default:
        break;
    }
}

LOAPlugin plugin;
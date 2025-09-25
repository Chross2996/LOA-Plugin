#include "stdafx.h"
#include "LOAPlugin.h"
#include <algorithm>
#include <cctype>
#include <unordered_map>
#include <unordered_set>
#include <chrono>

// Case-insensitive compare
bool EqualsIgnoreCase(const std::string& a, const std::string& b) {
    return std::equal(a.begin(), a.end(), b.begin(), b.end(),
        [](char a, char b) { return tolower(a) == tolower(b); });
}

const LOAEntry* MatchLoaEntry(const EuroScopePlugIn::CFlightPlan& fp, const std::unordered_set<std::string>& onlineControllers)
{
    if (!fp.IsValid() || !plugin.IsLOARelevantState(fp.GetState())) return nullptr;

    const char* planType = fp.GetFlightPlanData().GetPlanType();
    if (_stricmp(planType, "I") != 0) return nullptr;

    const std::string callsign = fp.GetCallsign();
    DWORD now = GetTickCount64();

    // Check if result is cached and still valid (5 seconds)
    auto tsIt = plugin.matchTimestamps.find(callsign);
    if (tsIt != plugin.matchTimestamps.end() && now - tsIt->second < 5000) {
        auto matchIt = plugin.matchedLOACache.find(callsign);
        if (matchIt != plugin.matchedLOACache.end()) {
            return matchIt->second;
        }
    }

    std::string origin = fp.GetFlightPlanData().GetOrigin();
    std::string destination = fp.GetFlightPlanData().GetDestination();
    std::string controller = fp.GetTrackingControllerId();

    const auto& routePoints = plugin.GetCachedRoutePoints(fp);

    auto matchIn = [&](const std::vector<LOAEntry>& entries) -> const LOAEntry* {
        for (const auto& entry : entries) {
            if (!entry.originAirports.empty() &&
                !plugin.MatchesAirport(entry.originAirportSet, entry.originAirportPrefixes, origin)) continue;

            if (!entry.destinationAirports.empty() &&
                !plugin.MatchesAirport(entry.destinationAirportSet, entry.destinationAirportPrefixes, destination)) continue;

            if (entry.requireNextSectorOnline && !entry.nextSectors.empty()) {
                bool online = std::any_of(entry.nextSectors.begin(), entry.nextSectors.end(),
                    [&](const std::string& ns) { return onlineControllers.count(ns) > 0; });
                if (!online) continue;
            }

            bool wpMatch = std::all_of(entry.waypoints.begin(), entry.waypoints.end(),
                [&](const std::string& wp) {
                    return std::any_of(routePoints.begin(), routePoints.end(),
                        [&](const std::string& r) { return EqualsIgnoreCase(r, wp); });
                });

            bool nextSectorMatch = entry.nextSectors.empty() || std::any_of(entry.nextSectors.begin(), entry.nextSectors.end(),
                [&](const std::string& ns) { return EqualsIgnoreCase(ns, controller); });

            if (wpMatch && nextSectorMatch)
                return &entry;
        }
        return nullptr;
        };

    const LOAEntry* result = nullptr;

    if ((result = matchIn(destinationLoas)) ||
        (result = matchIn(departureLoas)) ||
        (result = matchIn(lorArrivals)) ||
        (result = matchIn(lorDepartures))) {
        plugin.matchedLOACache[callsign] = result;
        plugin.matchTimestamps[callsign] = now;
        return result;
    }

    int clearedAltitude = fp.GetClearedAltitude();
    for (const auto& entry : fallbackLoas) {
        if (clearedAltitude < entry.minAltitudeFt) continue;

        if (!entry.destinationAirports.empty() &&
            !plugin.MatchesAirport(entry.destinationAirportSet, entry.destinationAirportPrefixes, destination)) continue;

        bool wpMatch = std::all_of(entry.waypoints.begin(), entry.waypoints.end(),
            [&](const std::string& wp) {
                return std::any_of(routePoints.begin(), routePoints.end(),
                    [&](const std::string& r) { return EqualsIgnoreCase(r, wp); });
            });

        if (wpMatch) {
            plugin.matchedLOACache[callsign] = &entry;
            plugin.matchTimestamps[callsign] = now;
            return &entry;
        }
    }

    // No match found — cache null to avoid re-evaluation within 5s
    plugin.matchedLOACache[callsign] = nullptr;
    plugin.matchTimestamps[callsign] = now;
    return nullptr;
}